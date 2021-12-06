/*
    Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010-2022 Belledonne Communications SARL, All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sstream>
#include <fstream>

#include <belle-sip/utils.h>

#include <flexisip/configmanager.hh>
#include <flexisip/flexisip-version.h>

#include "conference-address-generator.hh"
#include "registration-events/client.hh"
#include "conference.hh"
#include "utils/uri-utils.hh"

#include "conference-server.hh"

using namespace std;
using namespace std::chrono;
using namespace linphone;

namespace flexisip {

sofiasip::Home ConferenceServer::mHome;
ConferenceServer::Init ConferenceServer::sStaticInit;

void ConferenceServer::_init () {
	string bindAddress{};

	// Set config, transport, create core, etc
	auto cTransport = Factory::get()->createTransports();
	cTransport->setTcpPort(0);
	cTransport->setUdpPort(0);
	cTransport->setTlsPort(0);
	cTransport->setDtlsPort(0);

	// Flexisip config
	auto config = GenericManager::get()->getRoot()->get<GenericStruct>("conference-server");
	try {
		mTransport = SipUri{config->get<ConfigString>("transport")->read()};
		if (mTransport.empty()) throw sofiasip::InvalidUrlError{"", "empty URI"};

		bindAddress = mTransport.getHost();

		const auto& portStr = mTransport.getPort();
		auto port = !portStr.empty() ? stoi(portStr) : 5060;
		cTransport->setTcpPort(port);
	} catch (const sofiasip::InvalidUrlError& e) { // thrown by SipUri constructor and when mTransport is empty
		LOGF("ConferenceServer: Your configured conference transport(\"%s\") is not an URI.\nIf you have \"<>\" in "
		     "your transport, remove them.",
		     e.getUrl().c_str());
	}
	mCheckCapabilities = config->get<ConfigBoolean>("check-capabilities")->read();
	
	/* Read enabled media types (audio, video, text) */
	auto mediaTypes = config->get<ConfigStringList>("supported-media-types")->read();
	if (find(mediaTypes.begin(), mediaTypes.end(), "audio") != mediaTypes.end()) mMediaConfig.audioEnabled = true;
	if (find(mediaTypes.begin(), mediaTypes.end(), "video") != mediaTypes.end()) mMediaConfig.videoEnabled = true;
	if (find(mediaTypes.begin(), mediaTypes.end(), "text") != mediaTypes.end()) mMediaConfig.textEnabled = true;
	if (mMediaConfig.audioEnabled == false && mMediaConfig.videoEnabled == false && mMediaConfig.textEnabled == false){
		LOGF("ConferenceServer: no media types enabled. Check configuration file.");
	}

	// Core
	auto configLinphone = Factory::get()->createConfig("");
	configLinphone->setString("sip", "bind_address", bindAddress);
	configLinphone->setBool("misc", "conference_server_enabled", true);
	configLinphone->setBool("misc", "enable_one_to_one_chat_room", config->get<ConfigBoolean>("enable-one-to-one-chat-room")->read());

	if (mMediaConfig.textEnabled){
		string dbUri = config->get<ConfigString>("database-connection-string")->read();
		if (dbUri.empty()) LOGF("ConferenceServer: database-connection-string is not set. It is mandatory for handling text conferences.");
		configLinphone->setInt("misc", "hide_empty_chat_rooms", 0);
		configLinphone->setInt("misc", "hide_chat_rooms_from_removed_proxies", 0);
		configLinphone->setString("storage", "backend", config->get<ConfigString>("database-backend")->read());
		configLinphone->setString("storage", "uri", dbUri);
	}else{
		configLinphone->setString("storage", "uri", "null");
		
	}

// TODO: delete - set preferred layout to Grid
	configLinphone->setInt("misc", "conference_layout", static_cast<int>(linphone::ConferenceLayout::Grid));
	configLinphone->setInt("misc", "max_calls", 1000);
	configLinphone->setBool("sip", "reject_duplicated_calls", false);
	configLinphone->setInt("sound", "conference_rate", 48000);
	configLinphone->setBool("rtp", "symmetric", true);
	configLinphone->setBool("rtp", "rtcp_enabled", true);
	configLinphone->setBool("rtp", "rtcp_mux", true);
	configLinphone->setBool("video", "dont_check_codecs", true);
	configLinphone->setBool("net", "enable_nat_helper",false);// to make sure contact address is not fixed by belle-sip
	
	string uuid = readUuid();
	if (!uuid.empty()) configLinphone->setString("misc", "uuid", uuid); 

	mCore = linphone::Factory::get()->createCoreWithConfig(configLinphone, nullptr);

	mCore->enableRtpBundle(true);
	mCore->enableEchoCancellation(false);

	mCore->setUserAgent("Flexisip-conference", FLEXISIP_GIT_VERSION);
	mCore->addListener(shared_from_this());
	mCore->enableConferenceServer(true);
	mCore->setTransports(cTransport);

	// Make LinphoneCore to slice incoming LIME multipart messages in order
	// each forwarded message contains only one encrypted message instead
	// of having the encrypted version for each recipient.
	mCore->enableLimeX3Dh(true);

	mCore->enableLimeX3Dh(config->get<ConfigBoolean>("enable-lime")->read());
	mCore->setAudioPort(-1); // use random ports.
	mCore->setVideoPort(-1); // use random ports.
	mCore->setUseFiles(true); //No sound card shall be used in calls.
	enableSelectedCodecs(mCore->getAudioPayloadTypes(), { "opus", "speex"});
	enableSelectedCodecs(mCore->getVideoPayloadTypes(), { "VP8"});
	
	// Enable ICE (with host candidates only) so that the relay service of the proxies is bypassed.
	shared_ptr<linphone::NatPolicy> natPolicy = mCore->createNatPolicy();
	natPolicy->enableIce(true);
	mCore->setNatPolicy(natPolicy);

	loadFactoryUris();
	
	auto outboundProxy = config->get<ConfigString>("outbound-proxy")->read();
	auto outboundProxyAddress = Factory::get()->createAddress(outboundProxy);
	if (!outboundProxyAddress){
		LOGF("Invalid outbound-proxy value '%s'", outboundProxy.c_str());
	}
	bool defaultAccountSet = false;
	for (const auto &conferenceServerUris : mConfServerUris){
		auto factoryUri = Factory::get()->createAddress(conferenceServerUris.first);
		auto accountParams = mCore->createAccountParams();
		
		if (!conferenceServerUris.second.empty()){
			auto focusUri= Factory::get()->createAddress(conferenceServerUris.second);
			accountParams->setIdentityAddress(focusUri);
		}else{
			accountParams->setIdentityAddress(factoryUri);
		}
		accountParams->setServerAddress(outboundProxyAddress);
		accountParams->setRegisterEnabled(false);
		accountParams->setOutboundProxyEnabled(true);
		accountParams->setConferenceFactoryUri(factoryUri->asString());
		auto account = mCore->createAccount(accountParams);
		mCore->addAccount(account);
		if (!defaultAccountSet){
			defaultAccountSet = true;
			mCore->setDefaultAccount(account);
		}
		mLocalDomains.push_back(factoryUri->getDomain());
	}

	/* Get additional local domains */
	auto otherLocalDomains = config->get<ConfigStringList>("local-domains")->read();
	for (auto& domain : otherLocalDomains)
		mLocalDomains.emplace_back(move(domain));
	otherLocalDomains.clear();
	mLocalDomains.sort();
	mLocalDomains.unique();

	mRegEventClientFactory = make_shared<RegistrationEvent::ClientFactory>(mCore);

	Status err = mCore->start();
	if (err == -2) LOGF("Linphone Core couldn't start because the connection to the database has failed");
	if (err < 0) LOGF("Linphone Core starting failed");
	
	if (uuid.empty()){
		// In case no uuid was set in persistent state directory, take the one randomly choosen by Liblinphone.
		writeUuid(configLinphone->getString("misc", "uuid", ""));
	}else if (configLinphone->getString("misc", "uuid", "") != uuid){
		LOGF("Unconsistent uuid");
	}

	RegistrarDb::get()->addStateListener(shared_from_this());
	if (RegistrarDb::get()->isWritable()){
		bindAddresses();
	}
}

void ConferenceServer::enableSelectedCodecs(const std::list<std::shared_ptr<linphone::PayloadType>>& codecs, const std::list<std::string> &mimeTypes){
	for(auto codec : codecs){
		if (std::find(mimeTypes.begin(), mimeTypes.end(), codec->getMimeType()) != mimeTypes.end()) {
			codec->enable(true);
		}else{
			codec->enable(false);
		}
	}
}

void ConferenceServer::_run() {
	const auto start = high_resolution_clock::now();
	mCore->iterate();
	const auto stop = high_resolution_clock::now();
	const auto duration = duration_cast<milliseconds>(stop - start);
	if (duration > 100ms) {
		SLOGW << "Be careful mCore->iterate() took more than 100ms [" << duration.count() << " ms] and delay main loop";
	}
}

void ConferenceServer::_stop() {
	mCore->removeListener(shared_from_this());
	RegistrarDb::get()->removeStateListener(shared_from_this());
}

void ConferenceServer::loadFactoryUris() {
	auto config = GenericManager::get()->getRoot()->get<GenericStruct>("conference-server");
	auto conferenceFactoryUriSetting = config->get<ConfigString>("conference-factory-uri");
	auto conferenceFactoryUrisSetting = config->get<ConfigStringList>("conference-factory-uris");
	auto conferenceFocusUrisSetting = config->get<ConfigStringList>("conference-focus-uris");
	auto conferenceFactoryUri = conferenceFactoryUriSetting->read();
	auto conferenceFactoryUris = conferenceFactoryUrisSetting->read();
	auto conferenceFocusUris = conferenceFocusUrisSetting->read();

	if (!conferenceFactoryUri.empty()) conferenceFactoryUris.push_back(conferenceFactoryUri);
	if (conferenceFactoryUris.empty()) {
		SLOGI <<  conferenceFactoryUrisSetting->getCompleteName() << " parameter must be set!";
	}
	auto focus_it = conferenceFocusUris.begin();
	for (auto factoryUri :conferenceFactoryUris) {
		if (focus_it != conferenceFocusUris.end()) {
			mConfServerUris.push_back({factoryUri,*focus_it++});
		} else if (mMediaConfig.audioEnabled  || mMediaConfig.videoEnabled) {
			LOGF("Number of factory uri [%lu] must match number of focus uri [%lu]",conferenceFactoryUri.size(),conferenceFocusUris.size());
		} else {
			mConfServerUris.push_back({factoryUri,""});
		}
	}
	
}

void ConferenceServer::onRegistrarDbWritable(bool writable) {
	if (writable) bindAddresses();
}

void ConferenceServer::onChatRoomStateChanged(const shared_ptr<Core>& lc,
                                              const shared_ptr<ChatRoom>& cr,
                                              ChatRoom::State state) {
	if (state == ChatRoom::State::Instantiated) {
		mChatRooms.push_back(cr);
		cr->addListener(shared_from_this());
	} else if (state == ChatRoom::State::Deleted) {
		cr->removeListener(shared_from_this());
		mChatRooms.remove(cr);
	}
}

void ConferenceServer::onConferenceAddressGeneration (const shared_ptr<ChatRoom> & cr) {
	shared_ptr<Config> config = mCore->getConfig();
	shared_ptr<Address> confAddr = cr->getConferenceAddress()->clone();
	LOGI("Conference address is %s", confAddr->asString().c_str());
	shared_ptr<ConferenceAddressGenerator> generator = make_shared<ConferenceAddressGenerator>(
		cr,
		confAddr,
		getUuid(),
		mPath,
		this
	);
	generator->run();
}

void ConferenceServer::onParticipantRegistrationSubscriptionRequested(
    const shared_ptr<ChatRoom>& cr, const shared_ptr<const Address>& participantAddr) {
	mSubscriptionHandler.subscribe(cr, participantAddr);
}

void ConferenceServer::onParticipantRegistrationUnsubscriptionRequested(
    const shared_ptr<ChatRoom>& cr, const shared_ptr<const Address>& participantAddr) {
	mSubscriptionHandler.unsubscribe(cr, participantAddr);
}

void ConferenceServer::bindAddresses() {
	if (mAddressesBound) return;

	// Bind the conference factory address in the registrar DB
	bindFactoryUris();
	
	if (mMediaConfig.textEnabled){
		// Binding loaded chat room
		for (const auto &chatRoom : mCore->getChatRooms()) {
			if (chatRoom->getPeerAddress()->getUriParam("gr").empty()){
				LOGE("Skipping chatroom %s with no gruu parameter.", chatRoom->getPeerAddress()->asString().c_str());
				continue;
			}
			bindChatRoom(chatRoom->getPeerAddress()->asStringUriOnly(), mTransport.str(), chatRoom->getPeerAddress()->getUriParam("gr"), nullptr);
		}
	}
	if (mMediaConfig.audioEnabled || mMediaConfig.videoEnabled){
		/* Bind focus URIs */
		bindFocusUris();
		/* Initialize static conferences - to be removed */
		initStaticConferences();
	}
	mAddressesBound = true;
}

void ConferenceServer::bindFactoryUris() {
	class FakeListener : public ContactUpdateListener {
		void onRecordFound(const shared_ptr<Record>& r) override {
		}
		void onError() override {
		}
		void onInvalid() override {
		}
		void onContactUpdated(const shared_ptr<ExtendedContact>& ec) override {
			SLOGD << "ConferenceServer: ExtendedContact contactId=" << ec->contactId() << " callId=" << ec->callId();
		}
	};
	shared_ptr<FakeListener> listener = make_shared<FakeListener>();

	for (auto conferenceFactoryUri : mConfServerUris){
		try {
			BindingParameters parameter;
			sip_contact_t* sipContact = sip_contact_create(mHome.home(),
				reinterpret_cast<const url_string_t*>(url_make(mHome.home(), mTransport.str().c_str())), nullptr);
			SipUri factory(conferenceFactoryUri.first);
			
			
			parameter.callId = "CONFERENCE";
			parameter.path = mPath;
			parameter.globalExpire = numeric_limits<int>::max();
			parameter.alias = false;
			parameter.version = 0;

			RegistrarDb::get()->bind(
				factory,
				sipContact,
				parameter,
				listener
			);
			
		} catch (const sofiasip::InvalidUrlError &e) {
			LOGF("'conference-server' value isn't a SIP URI [%s]", e.getUrl().c_str());
		}
	}
}

void ConferenceServer::bindFocusUris() {
	class FocusListener : public ContactUpdateListener {
	public:
		FocusListener(const shared_ptr<Account> &account, const string &uuid) : mAccount(account), mUuid(uuid){
		}
		void onRecordFound(const shared_ptr<Record> &r) override {
			if (r->getExtendedContacts().empty()) {
				LOGF("Focus address bind failed.");
				return;
			}
			shared_ptr<ExtendedContact> ec = r->extractContactByUniqueId(UriUtils::grToUniqueId(mUuid));
			if (!ec){
				LOGA("Focus uri was not recorded in registrar database.");
				return;
			}
			url_t *pub_gruu = r->getPubGruu(ec, mHome.home());
			if (!pub_gruu) {
				LOGA("Focus binding does not have public gruu.");
				return;
			}
			shared_ptr<linphone::Address> gruuAddr = linphone::Factory::get()->createAddress(
				url_as_string(mHome.home(), pub_gruu));
			LOGI("Focus address [%s] is bound.", gruuAddr->asStringUriOnly().c_str());
			mAccount->setContactAddress(gruuAddr);
		}
		void onError() override {}
		void onInvalid() override {}
		void onContactUpdated(const shared_ptr<ExtendedContact> &ec) override {
			SLOGD << "ConferenceServer: ExtendedContact contactId=" << ec->contactId() << " callId=" << ec->callId();
		}
	private:
		shared_ptr<Account> mAccount;
		const string mUuid;
	};
	string uuid = getUuid();
	
	for (auto account : mCore->getAccountList()){
		BindingParameters parameter;
		auto identityAddress = account->getParams()->getIdentityAddress();
		auto factoryAddress = Factory::get()->createAddress(account->getParams()->getConferenceFactoryUri());
		
		if (identityAddress->equal(factoryAddress)) continue;
		
		sip_contact_t* sipContact = sip_contact_create(mHome.home(),
			reinterpret_cast<const url_string_t*>(url_make(mHome.home(), mTransport.str().c_str())),
		!uuid.empty() ? su_strdup(mHome.home(), ("+sip.instance=" + UriUtils::grToUniqueId(uuid) ).c_str()) : nullptr, nullptr);
		
		parameter.callId = "CONFERENCE";
		parameter.path = mPath;
		parameter.globalExpire = numeric_limits<int>::max();
		parameter.alias = false;
		parameter.version = 0;
		parameter.withGruu = true;

		SipUri focus(account->getParams()->getIdentityAddress()->asStringUriOnly());
		shared_ptr<FocusListener> listener = make_shared<FocusListener>(account, uuid);
		RegistrarDb::get()->bind(
			focus,
			sipContact,
			parameter,
			listener
		);
			
	}
}

void ConferenceServer::bindChatRoom(const string& bindingUrl,
                                    const string& contact,
                                    const string& gruu,
                                    const shared_ptr<ContactUpdateListener>& listener) {
	BindingParameters parameter;

	sip_contact_t* sipContact =
	    sip_contact_create(mHome.home(), reinterpret_cast<const url_string_t*>(url_make(mHome.home(), contact.c_str())),
	                       su_strdup(mHome.home(), ("+sip.instance=" + UriUtils::grToUniqueId(gruu)).c_str()), nullptr);

	parameter.callId = !gruu.empty() ? gruu : "dummy-callid";
	parameter.path = mPath;
	parameter.globalExpire = numeric_limits<int>::max();
	parameter.alias = false;
	parameter.version = 0;
	parameter.withGruu = true;

	RegistrarDb::get()->bind(SipUri(bindingUrl), sipContact, parameter, listener);
}

void ConferenceServer::onCallStateChanged(const std::shared_ptr<linphone::Core> & lc, 
						const std::shared_ptr<linphone::Call> & call, 
				  linphone::Call::State cstate, const std::string & message){

/*
	auto to = call->getToAddress();
	auto remoteParams = call->getRemoteParams();
	
	bool iceNegotiationOngoing = false;
	if (remoteParams->audioEnabled()) {
		auto audioStats = call->getAudioStats();
		auto iceState = audioStats->getIceState();
		iceNegotiationOngoing |= (iceState == linphone::IceState::InProgress);
	}
	if (remoteParams->videoEnabled()) {
		auto videoStats = call->getVideoStats();
		auto iceState = videoStats->getIceState();
		iceNegotiationOngoing |= (iceState == linphone::IceState::InProgress);
	}
	auto it = mConferences.find(to->getUsername());
	switch(cstate){
		case linphone::Call::State::IncomingReceived:
			if (it != mConferences.end()){
				const auto params = mCore->createCallParams(call);
				params->enableAudio(true);
				params->enableVideo(false);
				call->acceptWithParams(params);
			}else{
				call->decline(linphone::Reason::NotFound);
			}
		break;
		case linphone::Call::State::StreamsRunning:
			if (it != mConferences.end()){
				if (!iceNegotiationOngoing){
					(*it).second->addCall(call);
				}
			}else{
				LOGD("Unable to add participant [%s] to any conference", call->getRemoteAddress()->asString().c_str());
			}
		break;
		default:
		break;
	}
*/
}

void ConferenceServer::createConference(const shared_ptr<const linphone::Address> &address){
	mConferences[address->getUsername()] = make_shared<Conference>(*this, address);
	bindChatRoom(address->asStringUriOnly(), mTransport.str(), "", nullptr);
}

void ConferenceServer::initStaticConferences(){
	int i;

	shared_ptr<linphone::Account> account = mCore->getDefaultAccount();
	shared_ptr<const linphone::Address> identity = account->getParams()->getIdentityAddress();
	shared_ptr<linphone::Address> confUri = identity->clone();
	for (i = 0 ; i < 10 ; ++i){
		ostringstream ostr;
		ostr << "video-conference2-" << i;
		confUri->setUsername(ostr.str());
		createConference(confUri);
	}
}

ConferenceServer::Init::Init() {
	ConfigItemDescriptor items[] = {
	    {Boolean, "enabled",
	     "Enable conference server", /* Do we need this ? The systemd enablement should be sufficient. */
	     "true"},
	    {String, "transport", "URI where the conference server must listen. Only one URI can be specified.",
	     "sip:127.0.0.1:6064;transport=tcp"},
	    {StringList, "conference-factory-uris",
	     "List of SIP uris used by clients to create a conference. This implicitely defines the list of SIP domains "
	     "managed by the conference server. For example:\n"
	     "conference-factory-uris=sip:conference-factory@sip.linphone.org sip:conference-factory@sip.linhome.org",
	     ""},
	    {String, "outbound-proxy",
	     "The Flexisip proxy URI to which the conference server should sent all its outgoing SIP requests.",
	     "sip:127.0.0.1:5060;transport=tcp"},
	    {StringList, "local-domains",
	     "Domains managed by the local SIP service, ie domains for which user registration information "
	     "can be found directly from the local registrar database (redis database). "
	     "For external domains (not in this list), a 'reg' SUBSCRIBE (RFC3680) will be emitted."
	     "It is not necessary to list here domains that appear in the 'conference-factory-uris' property. "
	     "They are assumed to be local domains already.\n"
	     "Ex: local-domains=sip.linphone.org conf.linphone.org linhome.org",
	     ""},
	    {String, "database-backend",
	     "Choose the type of backend that linphone will use for the connection.\n"
	     "Depending on your Soci package and the modules you installed, the supported databases are: "
	     "`mysql`, `sqlite3`",
	     "mysql"},
	    {String, "database-connection-string",
	     "The configuration parameters of the backend.\n"
	     "The basic format is \"key=value key2=value2\". For a mysql backend, this "
	     "is a valid config: \"db=mydb user=user password='pass' host=myhost.com\".\n"
	     "Please refer to the Soci documentation of your backend, for instance: "
	     "http://soci.sourceforge.net/doc/3.2/backends/mysql.html"
	     "http://soci.sourceforge.net/doc/3.2/backends/sqlite3.html",
	     "db='mydb' user='myuser' password='mypass' host='myhost.com'"},
	    {Boolean, "check-capabilities",
	     "Whether the conference server shall check device capabilities before inviting them to a session.\n"
	     "The capability check is currently limited to Linphone client that put a +org.linphone.specs contact parameter"
	     " in order to indicate whether they support group chat and secured group chat.",
	     "true"},

	    // Deprecated paramters:
	    {String, "conference-factory-uri",
	     "uri where the client must ask to create a conference. For example:\n"
	     "conference-factory-uri=sip:conference-factory@sip.linphone.org",
	     ""},
	    {Boolean, "enable-one-to-one-chat-room", "Whether one-to-one chat room creation is allowed or not.", "true"},

	    config_item_end};

	auto uS = make_unique<GenericStruct>(
	    "conference-server",
	    "Flexisip conference server parameters. "
	    "The flexisip conference server is a user-agent that handles session-based chat (yes, text only at this time). "
	    "It requires a mysql database in order to persisently store chatroom state (participants and their devices). "
	    "It will use the Registrar backend (see section module::Registrar) to discover devices (or client instances) "
	    "of each participant.",
	    0);
	auto s = GenericManager::get()->getRoot()->addChild(move(uS));
	s->addChildrenValues(items);
	s->get<ConfigString>("conference-factory-uri")
	    ->setDeprecated("2020-09-30", "2.1.0",
	                    "Use 'conference-factory-uris' instead, that allows to declare multiple factory uris.");
	s->get<ConfigBoolean>("enable-one-to-one-chat-room")
	    ->setDeprecated("2022-09-21", "2.2.0", "This parameter will be forced to 'true' in further versions.");
}

string ConferenceServer::getUuidFilePath()const{
	return string(DEFAULT_VAR_LIB_DIR) + string("/flexisip/") + string(sUuidFile);
}

const string &ConferenceServer::readUuid(){
	ifstream fi;
	mUuid = "";
	string path = getUuidFilePath();
	fi.open(path);
	if (!fi.is_open()){
		LOGD("Cannot open uuid file %s: %s", path.c_str(), strerror(errno));
		return mUuid;
	}
	fi >> mUuid;
	fi.close();
	LOGD("Using uuid '%s'", mUuid.c_str());
	return mUuid;
}

void ConferenceServer::writeUuid(const string & uuid){
	ofstream fo;
	mUuid = uuid;
	string path = getUuidFilePath();
	fo.open(path);
	if (!fo.is_open()){
		LOGE("Cannot open uuid file %s: %s", path.c_str(), strerror(errno));
		return;
	}
	fo << uuid;
	fo.close();
}

std::string ConferenceServer::getUuid(){
	if (mUuid.empty()) mUuid = mCore->getConfig()->getString("misc", "uuid", "");
	return mUuid;
}

} // namespace flexisip
