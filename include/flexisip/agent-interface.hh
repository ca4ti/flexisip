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

#pragma once

#include <memory>
#include <string>

#include <sofia-sip/nta.h>
#include <sofia-sip/sip.h>
#include <sofia-sip/su_wait.h>
#include <sofia-sip/url.h>

#include "transaction.hh"

namespace flexisip {

class Agent;
class DomainRegistrationManager;
class Module;
class RequestSipEvent;
class ResponseSipEvent;
class SipEvent;

class Agent : public IncomingAgent, public OutgoingAgent, public std::enable_shared_from_this<Agent> {
public:
	virtual ~Agent() = default;

	virtual const std::shared_ptr<sofiasip::SuRoot>& getRoot() const noexcept = 0;
	virtual nta_agent_t* getSofiaAgent() const = 0;

	virtual const char* getServerString() const = 0;
	virtual const std::string& getUniqueId() const = 0;

	virtual std::pair<std::string, std::string> getPreferredIp(const std::string &destination) const = 0;
	virtual std::string getPreferredRoute() const = 0;
	virtual const url_t* getPreferredRouteUrl() const = 0;

	virtual const std::string& getRtpBindIp(bool ipv6 = false) const = 0;
	virtual const std::string& getPublicIp(bool ipv6 = false) const = 0;
	virtual const std::string& getResolvedPublicIp(bool ipv6 = false) const = 0;

	virtual const url_t* getNodeUri() const = 0;
	virtual const url_t* getClusterUri() const = 0;
	virtual const url_t* getDefaultUri() const = 0;

	virtual tport_t* getInternalTport() const = 0;
	virtual DomainRegistrationManager* getDRM() = 0;

	virtual bool isUs(const url_t* url, bool check_aliases = true) const = 0;
	virtual sip_via_t* getNextVia(sip_t* response) = 0;
	virtual int countUsInVia(sip_via_t* via) const = 0;

	virtual url_t* urlFromTportName(su_home_t* home, const tp_name_t* name) = 0;
	virtual void applyProxyToProxyTransportSettings(tport_t* tp) = 0;

	virtual std::shared_ptr<Module> findModule(const std::string& moduleName) const = 0;
	virtual std::shared_ptr<Module> findModuleByFunction(const std::string& moduleFunction) const = 0;

	using timerCallback = void (*)(void *unused, su_timer_t *t, void *data);
	virtual su_timer_t* createTimer(int milliseconds, timerCallback cb, void *data, bool repeating=true) = 0;
	virtual void stopTimer(su_timer_t* t) = 0;

	virtual void logEvent(const std::shared_ptr<SipEvent>& ev) = 0;
	virtual void incrReplyStat(int status) = 0;

	virtual void sendRequestEvent(std::shared_ptr<RequestSipEvent> ev) = 0;
	virtual void sendResponseEvent(std::shared_ptr<ResponseSipEvent> ev) = 0;
	virtual void injectRequestEvent(std::shared_ptr<RequestSipEvent> ev) = 0;
	virtual void injectResponseEvent(std::shared_ptr<ResponseSipEvent> ev) = 0;
	virtual void idle() = 0;
};

}; // namespace flexisip
