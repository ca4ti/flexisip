/*
    Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010-2023 Belledonne Communications SARL, All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "eventlogs.hh"

#include "eventlogs/sip-event-log.hh"
#include "flexisip/configmanager.hh"

#include "event-log-writer.hh"

using namespace std;

namespace flexisip {

EventLog::Init EventLog::evStaticInit;

EventLog::Init::Init() {
	ConfigItemDescriptor items[] = {
	    {Boolean, "enabled", "Enable event logs.", "false"},
	    {String, "logger", "Define logger for storing logs. It supports \"filesystem\" and \"database\".",
	     "filesystem"},
	    {String, "filesystem-directory",
	     "Directory where event logs are written as a filesystem (case when filesystem "
	     "output is choosed).",
	     "/var/log/flexisip"},
	    {String, "database-backend",
	     "Choose the type of backend that Soci will use for the connection.\n"
	     "Depending on your Soci package and the modules you installed, the supported databases are:"
	     "`mysql`, `sqlite3` and `postgresql`",
	     "mysql"},
	    {String, "database-connection-string",
	     "The configuration parameters of the backend.\n"
	     "The basic format is \"key=value key2=value2\". For a mysql backend, this "
	     "is a valid config: \"db=mydb user=user password='pass' host=myhost.com\".\n"
	     "Please refer to the Soci documentation of your backend, for instance: "
	     "http://soci.sourceforge.net/doc/master/backends/#supported-backends-and-features",
	     "db='mydb' user='myuser' password='mypass' host='myhost.com'"},
	    {Integer, "database-max-queue-size",
	     "Amount of queries that will be allowed to be queued before bailing password requests.\n"
	     "This value should be chosen accordingly with 'database-nb-threads-max', so that you have a "
	     "coherent behavior.\n"
	     "This limit is here mainly as a safeguard against out-of-control growth of the queue in the event of a flood "
	     "or big delays in the database backend.",
	     "100"},
	    {Integer, "database-nb-threads-max",
	     "Maximum number of threads for writing in database.\n"
	     "If you get a `database is locked` error with sqlite3, you must set this variable to 1.",
	     "10"},

	    // Deprecated parameters
	    {String, "dir",
	     "Directory where event logs are written as a filesystem (case when filesystem output is choosed).",
	     "/var/log/flexisip"},
	    config_item_end};

	auto uEv = make_unique<GenericStruct>(
	    "event-logs",
	    "Event logs contain per domain and user information about processed registrations, calls and messages.\n"
	    "See: https://wiki.linphone.org/xwiki/wiki/public/view/Flexisip/Event%20logs%20and%20queries/ for architecture "
	    "and queries.",
	    0);
	auto ev = GenericManager::get()->getRoot()->addChild(move(uEv));
	ev->addChildrenValues(items);
	ev->get<ConfigString>("dir")->setDeprecated({"2020-02-19", "2.0.0", "Replaced by 'filesystem-directory'"});
}

EventLog::EventLog(const sip_t* sip)
    : SipEventLog(*sip), mUA{sip->sip_user_agent ? sip_user_agent_dup(mHome.home(), sip->sip_user_agent) : nullptr},
      mDate{time(nullptr)}, mCallId{sip->sip_call_id->i_id} {
}

RegistrationLog::RegistrationLog(const sip_t* sip, const sip_contact_t* contacts) : EventLog(sip) {
	mType = (sip->sip_expires && sip->sip_expires->ex_delta == 0) ? Type::Unregister // REVISIT not 100% exact.
	                                                              : Type::Register;

	mContacts = sip_contact_dup(mHome.home(), contacts);
}

void RegistrationLog::write(EventLogWriter& writer) const {
	writer.write(*this);
}

void CallLog::write(EventLogWriter& writer) const {
	writer.write(*this);
}

void MessageLog::write(EventLogWriter& writer) const {
	writer.write(*this);
}

AuthLog::AuthLog(const sip_t* sip, bool userExists)
    : EventLog(sip), mMethod{sip->sip_request->rq_method_name}, mUserExists{userExists} {
	setOrigin(sip->sip_via);
}

void AuthLog::setOrigin(const sip_via_t* via) {
	const char* protocol = strchr(via->v_protocol, '/') + 1;
	const char* port = via->v_rport ? via->v_rport : via->v_port;
	const char* ip = via->v_received ? via->v_received : via->v_host;

	protocol = strchr(protocol, '/') + 1;

	mOrigin = url_format(mHome.home(), "sip:%s", ip);
	if (!mOrigin) {
		LOGE("AuthLog: invalid via with host %s", ip);
		mOrigin = url_format(mHome.home(), "sip:invalid.host");
	}
	if (port) {
		mOrigin->url_port = su_strdup(mHome.home(), port);
	}
	if (protocol) {
		mOrigin->url_params = su_sprintf(mHome.home(), "transport=%s", protocol);
	}
}

void AuthLog::write(EventLogWriter& writer) const {
	writer.write(*this);
}

CallQualityStatisticsLog::CallQualityStatisticsLog(const sip_t* sip)
    : EventLog(sip), mReport{sip->sip_payload && sip->sip_payload->pl_data ? sip->sip_payload->pl_data : nullptr} {
}

void CallQualityStatisticsLog::write(EventLogWriter& writer) const {
	writer.write(*this);
}

} // namespace flexisip
