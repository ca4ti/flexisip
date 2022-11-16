/*
	Flexisip, a flexible SIP proxy server with media capabilities.
	Copyright (C) 2010-2015  Belledonne Communications SARL, All rights reserved.

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

#include "domain-registrations.hh"
#include "agent.hh"
#include "module.hh"

#include <sofia-sip/nta_stateless.h>
#include <sofia-sip/nth.h>
#include <sofia-sip/sip.h>
#include <sofia-sip/sip_protos.h>
#include <sofia-sip/sip_util.h>
#include <sofia-sip/sip_tag.h>
#include <sofia-sip/nta_tport.h>
#include <sofia-sip/auth_client.h>

#include <fstream>
#include <sstream>

using namespace std;

DomainRegistrationManager::DomainRegistrationManager(Agent *agent) : mAgent(agent) {
	GenericManager *mgr = GenericManager::get();
	mDomainRegistrationArea = new GenericStruct(
		"inter-domain-connections",
		"Inter domain connections is a set of feature allowing to dynamically connect several flexisip servers "
		"together in order to manage SIP routing at local and global"
		" scope. Let's suppose you have two SIP network a.example.net and b.example.net run privately and "
		"independently (no one from a.example.net "
		"needs to call someone at b.example.net). However, when people from a and b are outside of their network, they "
		"register to a worldwide available "
		"flexisip instance running on 'global.example.net'. It is then possible to:\n"
		"* have calls made within a.example.net routed locally and sent to global.example.net in order to reach users "
		"inside and outside of a's network."
		" Example: 1@a.example.net calls 2@a.example.net. If 2 is registered on a.example.net then the call is routed "
		"locally. On the contrary if 2 is"
		" absent and registered, the call is then sent to global.example.net and then routed by the global proxy.\n"
		"* when global.example.net receives a call from a user not within its native network (ex: 1@a.example.net "
		"calls 2@a.example.net), "
		"it can route this call to the proxy that is responsible for managing the local domain (a.example.net).\n"
		"This system is dynamic, that is the physical IP address of a and b network can change (dynamic ip address)\n."
		"This scenario is achieved with two key features:\n"
		"* a.example.net sends a REGISTER to global.example.net to indicate that it is the responsible for the entire "
		"domain a.example.net."
		" The global.example.net authenticates this REGISTER thanks to TLS client certificate presented by "
		"a.example.net.\n"
		"* global.example.net is configured to accept this domain registration and route all calls it receives "
		"directly and estinated to a.example.net domain"
		" through the connection established by a.example.net during the domain registration.",
		ModuleInfoBase::InterDomainConnections);

	mgr->getRoot()->addChild(mDomainRegistrationArea);

	ConfigItemDescriptor configs[] = {
		{Boolean, "accept-domain-registrations", "Whether flexisip shall accept registrations for entire domains",
		 "false"},
		{Boolean, "assume-unique-domains",
		 "Whether flexisip shall assume that there is a unique server per registered domain, which allows"
		 " to clean old registrations and simplifies the routing logic.",
		 "false"},
		{String, "domain-registrations",
		 "Path to a text file describing the domain registrations to make. This file must contains lines like:\n"
		 " <local domain name> <SIP URI of proxy/registrar where to send the domain REGISTER>\n"
		 " where:\n"
		 " <local domain name> is a domain name managed locally by this proxy\n"
		 " <SIP URI of proxy/registrar> is the SIP URI where the domain registration will be sent. The special uri "
		 "parameter"
		 " 'tls-certificate-dir' is understood in order to specify a TLS client certificate to present to the remote "
		 "proxy.\n"
		 " If the file is absent or empty, no registrations are done.",
		 "/etc/flexisip/domain-registrations.conf"},
		{Boolean, "verify-server-certs",
		 "When submitting a domain registration to a server over TLS, verify the certificate presented by the server. "
		 "Disabling this option is only for test, because it is a security flaw",
		 "true"},
		{Integer, "keepalive-interval",
			"Interval in seconds for sending \\r\\n\\r\\n keepalives through the outgoing domain registration connection."
			"A value of zero disables keepalives.",
			"30"},
		{Integer, "ping-pong-timeout-delay",
			"Delay in seconds after which TCP/TLS connections will be considered as broken if no CRLF pong has been"
			"received from the remote peer. A delay of 0 means that no pong is expected after ping.\n"
			"Warning: This parameter must be strictly lower than “keepalive-interval”.",
		"0"},
		{Integer, "reconnection-delay",
			"Delay in seconds before creating a new connection after connection is known as broken. Set '0' in order the "
			"connection be recreated immediately.",
		"5"},
		{Boolean, "relay-reg-to-domains",
		 "Relay UAs registration requests to the flexisip server which has registered that specific user's domain",
		"false"},
		config_item_end};

	mDomainRegistrationArea->addChildrenValues(configs);


}

DomainRegistrationManager::~DomainRegistrationManager() {
	if(mNbRegistration > 0) {
		LOGD("Starting domain un-registration");
		for(auto &registration : mRegistrations) {
			registration->stop();
		}
		mTimer = NULL;
		mTimer = mAgent->createTimer(5000, unregisterTimeout, mAgent->getRoot());
		su_root_run(mAgent->getRoot()); // Correctly wait for domain un-registration
	}
}

int DomainRegistrationManager::load() {
	ifstream ifs;
	string configFile;
	int lineIndex = 0;

	GenericStruct *domainRegistrationCfg =
		GenericManager::get()->getRoot()->get<GenericStruct>("inter-domain-connections");
	configFile = domainRegistrationCfg->get<ConfigString>("domain-registrations")->read();


	mVerifyServerCerts = domainRegistrationCfg->get<ConfigBoolean>("verify-server-certs")->read();
	
	auto keepAliveIntervalCfg = domainRegistrationCfg->get<ConfigInt>("keepalive-interval");
	auto pingPongTimeoutDelayCfg = domainRegistrationCfg->get<ConfigInt>("ping-pong-timeout-delay");
	auto reconnectionDelayCfg = domainRegistrationCfg->get<ConfigInt>("reconnection-delay");
	mKeepaliveInterval = chrono::seconds{keepAliveIntervalCfg->read()};
	mPingPongTimeoutDelay = chrono::seconds{pingPongTimeoutDelayCfg->read()};
	if (mPingPongTimeoutDelay >= mKeepaliveInterval) {
		LOGF("'%s' value [%us] must be strictly lower than '%s' [%us]",
			pingPongTimeoutDelayCfg->getName().c_str(), static_cast<unsigned>(mPingPongTimeoutDelay.count()),
			keepAliveIntervalCfg->getName().c_str(), static_cast<unsigned>(mKeepaliveInterval.count()));
	}
	mReconnectionDelay = chrono::seconds{reconnectionDelayCfg->read()};
	
	if (configFile.empty())
		return 0;

	ifs.open(configFile);
	if (!ifs.is_open()) {
		LOGE("Cannot open domain registration configuration file '%s'", configFile.c_str());
		return -1;
	}

	LOGD("Loading domain registration configuration from %s", configFile.c_str());
	do {
		SofiaAutoHome home;
		string line;
		string domain, uri, pwd;
		bool is_a_comment = false;
		getline(ifs, line);

		for (size_t i = 0; i < line.size(); ++i) {
			// skip spaces or comments
			if (isblank(line[i]))
				continue;
			if (line[i] == '#')
				is_a_comment = true;
			else
				break;
		}
		if (is_a_comment)
			continue;
		istringstream istr(line);
		istr >> domain;
		istr >> uri;
		istr >> pwd;
		if (domain.empty())
			continue; /*empty line */
		if (uri.empty()) {
			LOGE("Empty URI in domain registration definition.");
			goto error;
		}
		if (pwd.empty()) {
			pwd = "";
		}
		if (uri[0] == '<') uri = uri.substr(1, uri.size()-2);
		url_t *url = url_make(home.home(), uri.c_str());
		if (!url) {
			LOGE("Bad URI '%s' in domain registration definition.", uri.c_str());
			goto error;
		}

		/*extract the certificate directory parameter if given, and remove it before passing the URI to the
		 * DomainRegistration object*/
		char clientCertdir[256] = {0};
		if (url_param(url->url_params, "tls-certificates-dir", clientCertdir, sizeof(clientCertdir)) > 0) {
			url->url_params = url_strip_param_string(su_strdup(home.home(), url->url_params), "tls-certificates-dir");
		}
		auto dr = make_shared<DomainRegistration>(*this, domain, pwd, url, clientCertdir, lineIndex);
		lineIndex++;
		mRegistrations.push_back(dr);
	} while (!ifs.eof() && !ifs.bad());
	for_each(mRegistrations.begin(), mRegistrations.end(), mem_fn(&DomainRegistration::start));
	return 0;
error:
	LOGF("Syntax error parsing domain registration configuration file '%s'", configFile.c_str());
	return -1;
}

bool DomainRegistrationManager::isUs(const url_t *url) const {
	for (auto it = mRegistrations.begin(); it != mRegistrations.end(); ++it) {
		const shared_ptr<DomainRegistration> &dr = *it;
		if (dr->isUs(url))
			return TRUE;
	}
	return FALSE;
}

const url_t *DomainRegistrationManager::getPublicUri(const tport_t *tport) const {
	for (auto it = mRegistrations.begin(); it != mRegistrations.end(); ++it) {
		const shared_ptr<DomainRegistration> &dr = *it;
		if (dr->hasTport(tport))
			return dr->getPublicUri();
	}
	return NULL;
}

DomainRegistration::DomainRegistration(DomainRegistrationManager &mgr, const string &localDomain, const string &pwd,
									   const url_t *parent_proxy, const string &clientCertdir, int lineIndex)
	: mManager(mgr) {
	char transport[64] = {0};
	tp_name_t tpn = {0};
	bool usingTls;
	int verifyPolicy = mgr.mVerifyServerCerts ? TPTLS_VERIFY_OUT | TPTLS_VERIFY_SUBJECTS_OUT : TPTLS_VERIFY_NONE;
	nta_agent_t *agent = mManager.mAgent->getSofiaAgent();

	su_home_init(&mHome);
	mFrom = url_format(&mHome, "%s:%s", parent_proxy->url_type == url_sips ? "sips" : "sip", localDomain.c_str());
	mProxy = url_hdup(&mHome, parent_proxy);
	mPwd = pwd;
	url_param(parent_proxy->url_params, "transport", transport, sizeof(transport) - 1);

	usingTls = parent_proxy->url_type == url_sips || strcasecmp(transport, "tls") == 0;

	if (usingTls && !clientCertdir.empty()) {
		string mainTlsCertsDir = GenericManager::get()->getRoot()->get<GenericStruct>("global")->get<ConfigString>("tls-certificates-dir")->read();
		if (strcmp(mainTlsCertsDir.c_str(), clientCertdir.c_str()) == 0) {
			// Certs dir is the same as for the existing tport
			LOGD("Domain registration certificates are the same as the one for existing tports, let's use them");
			mPrimaryTport = nta_agent_tports(agent);
		} else {
			list<string> canons;
			tport_t *primaries = tport_primaries(nta_agent_tports(agent));
			for (tport_t *tport = primaries; tport != NULL; tport = tport_next(tport)) {
				const tp_name_t *name;
				name = tport_name(tport);
				if (strcmp(name->tpn_proto, "tls") == 0) {
					canons.push_back(name->tpn_canon);
				}
			}
			for (list<string>::iterator it = canons.begin(); it != canons.end(); ++it) {
				url_t *tportUri = NULL;
				tportUri = url_format(&mHome, "sips:%s:0", (*it).c_str());
				/* need to add a new tport because we want to use a specific certificate for this connection*/
				nta_agent_add_tport(agent, (url_string_t *)tportUri, TPTAG_CERTIFICATE(clientCertdir.c_str()),
									TPTAG_IDENT(localDomain.c_str()),
									TPTAG_TLS_VERIFY_POLICY(verifyPolicy), TAG_END());
				tpn.tpn_ident = localDomain.c_str();
				mPrimaryTport = tport_by_name(nta_agent_tports(agent), &tpn);
				if (!mPrimaryTport) {
					LOGF("Could not find the tport we just added in the agent.");
				}
			}
		}
	} else {
		/*otherwise we can use the agent's already existing transports*/
		mPrimaryTport = nta_agent_tports(agent);
	}

	mLeg = nta_leg_tcreate(agent, sLegCallback, (nta_leg_magic_t *)this, NTATAG_METHOD("REGISTER"),
						   SIPTAG_FROM(sip_from_create(&mHome, (url_string_t *)mFrom)),
						   SIPTAG_TO(sip_to_create(&mHome, (url_string_t *)mFrom)), URLTAG_URL(mProxy), TAG_END());
	if (!mLeg) {
		LOGF("Could not create leg");
	}
	mCurrentTport = NULL;
	mTimer = NULL;
	mExternalContact = NULL;
	mOutgoing = NULL;

	ostringstream domainRegistrationStatName;
	domainRegistrationStatName<<"registration-status-"<<lineIndex;
	ostringstream domainRegistrationStatHelp;
	domainRegistrationStatHelp<<"Domain registration status for "<< localDomain;
	mRegistrationStatus = mgr.mDomainRegistrationArea->createStat(domainRegistrationStatName.str(), domainRegistrationStatHelp.str());
}

tport_t *DomainRegistrationManager::lookupTport(const url_t *destUrl){
	for (auto it = mRegistrations.begin(); it != mRegistrations.end(); ++it) {
		const shared_ptr<DomainRegistration> &dr = *it;
		if (url_cmp(dr->getProxy(), destUrl) == 0){
			return dr->getTport();
		}
	}
	return nullptr;
}

bool DomainRegistration::hasTport(const tport_t *tport) const {
	return tport == mCurrentTport && mCurrentTport != NULL;
}

const url_t *DomainRegistration::getPublicUri() const {
	return mExternalContact->m_url;
}

int DomainRegistration::sLegCallback(nta_leg_magic_t *ctx, nta_leg_t *leg, nta_incoming_t *incoming,
									 const sip_t *request) {
	LOGE("legCallback called");
	return 500;
}

void DomainRegistration::sRefreshRegistration(su_root_magic_t *magic, su_timer_t *timer, su_timer_arg_t *arg) {
	static_cast<DomainRegistration *>(arg)->start();
}

void DomainRegistration::sRefreshUnregistration(su_root_magic_t *magic, su_timer_t *timer, su_timer_arg_t *arg) {
	static_cast<DomainRegistration *>(arg)->stop();
}

int DomainRegistration::getExpires(nta_outgoing_t *orq, const sip_t *response) {
	int expires;
	if (response->sip_expires)
		return response->sip_expires->ex_delta;
	if (response->sip_contact && response->sip_contact->m_expires) {
		expires = atoi(response->sip_contact->m_expires);
		if (expires > 0)
			return expires;
	}
	msg_t *req = nta_outgoing_getrequest(orq);
	sip_t *sip = (sip_t *)msg_object(req);
	expires = sip->sip_expires->ex_delta;
	msg_unref(req); // because nta_outgoing_getrequest() gives a new reference.
	return expires;
}

void DomainRegistration::onConnectionBroken(tport_t *tport, msg_t *msg, int error) {
	using namespace std::chrono;

	
	if (tport == mCurrentTport) {
		/* Cleanup current tport here, to force creation of a new one upon next registraion attempt */
		LOGD("Current tport is broken");
		cleanCurrentTport();
	}
	
	// restart registration...
	if (mTimer) {
		su_timer_destroy(mTimer);
		mTimer = NULL;
	}
	
	const auto& nextSchedule = mManager.mReconnectionDelay;
	
	mTimer = su_timer_create(su_root_task(mManager.mAgent->getRoot()), 0);
	SLOGD << "Scheduling next domain register refresh for " << mFrom->url_host << " in "
		<< duration_cast<seconds>(nextSchedule).count() << " seconds";
	su_timer_set_interval(mTimer, &DomainRegistration::sRefreshRegistration, this, (su_duration_t)nextSchedule.count() * 1000);
	SLOGD << "DomainRegistration::onConnectionBroken(), restarting registration in "
		<< duration_cast<seconds>(nextSchedule).count() << " seconds";
	mRegistrationStatus->set(503);
}

void DomainRegistration::sOnConnectionBroken(tp_stack_t *stack, tp_client_t *client, tport_t *tport, msg_t *msg,
											 int error) {
	reinterpret_cast<DomainRegistration *>(client)->onConnectionBroken(tport, msg, error);
}

void DomainRegistration::responseCallback(nta_outgoing_t *orq, const sip_t *resp) {
	int nextSchedule;
	SofiaAutoHome home;

	if (mTimer) {
		su_timer_destroy(mTimer);
		mTimer = NULL;
	}
	mTimer = su_timer_create(su_root_task(mManager.mAgent->getRoot()), 0);
	if (resp) {
		msg_t *msg = nta_outgoing_getresponse(orq);
		SLOGD << "DomainRegistration::responseCallback(): receiving response:" << endl
			  << msg_as_string(home.home(), msg, msg_object(msg), 0, NULL);
		msg_unref(msg);
	}

	mRegistrationStatus->set(resp ? resp->sip_status->st_status : 408); /*if no response, it is a timeout*/

	if (!resp || resp->sip_status->st_status != 200) {
		/*the registration failed for whatever reason. Retry shortly.*/
		if (!resp){
			nextSchedule = 1;
			SLOGUE << "Domain registration error for " << url_as_string(home.home(), mFrom);
		}else{
			nextSchedule = (resp->sip_status->st_status == 401) ? 0 : 30;
			SLOGUE << "Domain registration error for " << url_as_string(home.home(), mFrom) << " : " << resp->sip_status->st_status;
		}

		int expire = resp ? getExpires(orq, resp) : -1;
		if(expire > 0) {
			LOGD("Domain registration for %s failed, will retry in %i seconds", mFrom->url_host, nextSchedule);
			su_timer_set_interval(mTimer, &DomainRegistration::sRefreshRegistration, this,
								  (su_duration_t)nextSchedule * 1000);
		} else if(expire == 0) {
			LOGD("Domain un-registration for %s failed, will retry in %i seconds", mFrom->url_host, nextSchedule);
			su_timer_set_interval(mTimer, &DomainRegistration::sRefreshUnregistration, this,
								  (su_duration_t)nextSchedule * 1000);
		}

		if (!resp){
			if (mCurrentTport){
				LOGD("No domain registration response, connection might be broken. Shutting down current connection.");
				tport_shutdown(mCurrentTport, 2);
				return;
			}
		} else if (resp->sip_status->st_status == 401) {
			const sip_t *sip = resp;
			const char* user = "";
			const char* realm = msg_params_find(sip->sip_www_authenticate->au_params, "realm=");
			const char* pwd = mPwd.c_str();
			msg_t *msg = nta_outgoing_getrequest(orq);
			auth_client_t *aucs = NULL;
			auc_challenge(&aucs, &mHome, sip->sip_www_authenticate,
								  sip_authorization_class);
			auc_all_credentials(&aucs, "DIGEST", realm, user, pwd);
			sip = sip_object(msg);
			msg_header_t *return_headers = nullptr;
			auc_authorization_headers(&aucs,&mHome,"REGISTER",(url_t *)sip->sip_request->rq_url,
										 sip->sip_payload,
										 &return_headers);
			mSip = return_headers;
			msg_unref(msg);
			setCurrentTport(nta_outgoing_transport(orq));
		}
	} else { // 200 OK
		int expire = getExpires(orq, resp);
		const char *domain = mFrom->url_host;
		if(expire > 0) {
			if(!(find(mManager.mRegistrationList.begin(), mManager.mRegistrationList.end(), domain) != mManager.mRegistrationList.end())) {
				mManager.mNbRegistration++;
				mManager.mRegistrationList.push_back(domain);
				LOGD("Incrementing number of domain registration to : %d.", mManager.mNbRegistration);

			}
		} else {
			if(find(mManager.mRegistrationList.begin(), mManager.mRegistrationList.end(), domain) != mManager.mRegistrationList.end()) {
				mManager.mNbRegistration--;
				mManager.mRegistrationList.erase(find(mManager.mRegistrationList.begin(), mManager.mRegistrationList.end(), domain));
				LOGD("Decrementing number of domain registration to : %d.", mManager.mNbRegistration);
			}
		}
		tport_t *tport = nta_outgoing_transport(orq);
		mSupportPongs = !!sip_has_supported(resp->sip_supported, "outbound");

		cleanCurrentTport();
		setCurrentTport(tport);
		nextSchedule = ((expire * 90) / 100) + 1;
		if(expire > 0) {
			LOGD("Scheduling next domain register refresh for %s in %i seconds", mFrom->url_host, nextSchedule);
			su_timer_set_interval(mTimer, &DomainRegistration::sRefreshRegistration, this,
											  (su_duration_t)nextSchedule * 1000);
		}
		/*store contact sent in response, as it gives information about our public IP/port*/
		if (resp->sip_contact) {
			if (mExternalContact) {
				su_free(&mHome, mExternalContact);
			}
			mExternalContact = sip_contact_dup(&mHome, resp->sip_contact);
		}
		if(mManager.mNbRegistration <= 0) {
			LOGD("Quiting domain registration");
			su_timer_destroy(mTimer);
			mTimer = NULL;
			su_root_break(mManager.mAgent->getRoot());
		}
	}
}

int DomainRegistration::sResponseCallback(nta_outgoing_magic_t *ctx, nta_outgoing_t *orq, const sip_t *resp) {
	reinterpret_cast<DomainRegistration *>(ctx)->responseCallback(orq, resp);
	return 0;
}

DomainRegistration::~DomainRegistration() {
	su_home_deinit(&mHome);
}

void DomainRegistration::setContact(msg_t *msg) {
	sip_t *sip = (sip_t *)msg_object(msg);
	if (sip->sip_contact == NULL) {
		sip->sip_contact = sip_contact_create(msg_home(msg), (url_string_t *)mFrom, NULL);
	}
}

void DomainRegistration::sendRequest() {
	/* Re-use current transport, whenever possible */
	auto tport = mCurrentTport ? mCurrentTport : mPrimaryTport;

	if (mTimer) {
		su_timer_destroy(mTimer);
		mTimer = NULL;
	}

	auto msg = nta_msg_create(mManager.mAgent->getSofiaAgent(), 0);
	if (nta_msg_request_complete(msg, mLeg, sip_method_register, nullptr, (url_string_t*)mProxy) != 0) {
		LOGE("nta_msg_request_complete() failed");
	}
	auto sip = sip_object(msg);
	msg_header_insert(msg, msg_object(msg), (msg_header_t*)sip_expires_create(msg_home(msg), mExpires.count()));

	if (mOutgoing) {
		msg_t* respMsg = nta_outgoing_getresponse(mOutgoing);
		if (respMsg) {
			auto* respSip = reinterpret_cast<sip_t*>(msg_object(respMsg));
			if (respSip->sip_status->st_status == 401) {
				/* Add authorization headers if last response was a 401 */
				const char* user = "";
				const char* realm = msg_params_find(respSip->sip_www_authenticate->au_params, "realm=");
				const char* pwd = mPwd.c_str();
				msg_header_t* authHeaders;
				auth_client_t* aucs = nullptr;
				auc_challenge(&aucs, msg_home(msg), respSip->sip_www_authenticate, sip_authorization_class);
				auc_all_credentials(&aucs, "DIGEST", realm, user, pwd);
				auc_authorization_headers(&aucs, msg_home(msg), "REGISTER", (url_t*)sip->sip_request->rq_url,
				                          sip->sip_payload, &authHeaders);
				msg_header_insert(msg, msg_object(msg), authHeaders);
			}
			msg_unref(respMsg);
		}
	}
	setContact(msg);
	msg_header_insert(msg, msg_object(msg),
	                  reinterpret_cast<msg_header_t*>(sip_supported_make(msg_home(msg), "outbound")));
	sip_complete_message(msg);
	msg_serialize(msg, msg_object(msg));

	SofiaAutoHome home;
	LOGD("Domain registration about to be sent:\n%s",
	     msg_as_string(home.home(), msg, msg_object(msg), 0, nullptr));

	if (mOutgoing) {
		nta_outgoing_destroy(mOutgoing);
	}
	mOutgoing = nta_outgoing_mcreate(mManager.mAgent->getSofiaAgent(), sResponseCallback, (nta_outgoing_magic_t*)this,
	                                 nullptr, msg, NTATAG_TPORT(tport), TAG_END());
	if (!mOutgoing) {
		LOGE("Could not create outgoing transaction");
		return;
	}
}

void DomainRegistration::start() {
	sendRequest();
}

void DomainRegistration::setCurrentTport(tport_t* tport) {
	using namespace std::chrono;

	if (!tport) return;
	if (mCurrentTport == tport) return;

	/*
	 * We cast the duration here because SofiaSip requires that those be express in milliseconds. Thus, the code
	 * won't be broken if we change the type of mKeepaliveInterval or mPingPongTimeoutDelay.
	 */
	auto keepAliveInterval = duration_cast<milliseconds>(mManager.mKeepaliveInterval);
	unsigned int pingPongTimeoutDelay =  mSupportPongs ?(duration_cast<milliseconds>(mManager.mPingPongTimeoutDelay)).count() : 0;
	cleanCurrentTport();
	mCurrentTport = tport;
	if (pingPongTimeoutDelay > 0) LOGD("Enabling PING/PONG for broken connection detection.");
	tport_set_params(tport, TPTAG_SDWN_ERROR(1), TPTAG_KEEPALIVE(keepAliveInterval.count()),
	                 TPTAG_PINGPONG(pingPongTimeoutDelay), TAG_END());
	mPendId = tport_pend(tport, nullptr, &DomainRegistration::sOnConnectionBroken, (tp_client_t*)this);
}

void DomainRegistration::cleanCurrentTport() {
	if (mCurrentTport) {
		tport_release(mCurrentTport, mPendId, NULL, NULL, (tp_client_t *)this, 0);
		tport_unref(mCurrentTport);
		mCurrentTport = NULL;
		mPendId = 0;
	}
}

void DomainRegistration::stop() {
	msg_t *msg;
	cleanCurrentTport();
	if (mTimer) {
		su_timer_destroy(mTimer);
		mTimer = NULL;
	}

	msg = nta_msg_create(mManager.mAgent->getSofiaAgent(), 0);
	if (nta_msg_request_complete(msg, mLeg, sip_method_register, NULL, (url_string_t *)mProxy) != 0) {
		LOGE("nta_msg_request_complete() failed");
	}
	if(mSip) {
		msg_header_insert(msg, msg_object(msg), msg_header_copy(&mHome, mSip));
		mSip = NULL;
	}
	msg_header_insert(msg, msg_object(msg), (msg_header_t *)sip_expires_create(msg_home(msg), 0));
	setContact(msg);
	sip_complete_message(msg);
	msg_serialize(msg, msg_object(msg));
	su_home_t home;
	su_home_init(&home);
	LOGD("Domain un-registration about to be sent:\n%s", msg_as_string(&home, msg, msg_object(msg), 0, NULL));
	su_home_deinit(&home);

	nta_outgoing_t *outgoing =
	nta_outgoing_mcreate(mManager.mAgent->getSofiaAgent(), sResponseCallback, (nta_outgoing_magic_t *)this, NULL,
						 msg, NTATAG_TPORT(mPrimaryTport), TAG_END());
	if (!outgoing) {
		LOGE("Could not create outgoing transaction");
		return;
	}
}

bool DomainRegistration::isUs(const url_t *url) {
	if (mExternalContact) {
		return ModuleToolbox::urlTransportMatch(url, mExternalContact->m_url);
	}
	return false;
}
