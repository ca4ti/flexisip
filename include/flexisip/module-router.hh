/*
    Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010-2021  Belledonne Communications SARL, All rights reserved.

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

#include "fork-context/fork-context.hh"

#include "flexisip/module.hh"
#include "fork-context/fork-context-listener.hh"

namespace flexisip {

class Agent;
struct ExtendedContact;
class OnContactRegisteredListener;
class Record;

struct ModuleRouterPrivAttr;

struct RouterStats {
	std::unique_ptr<StatPair> mCountForks;
	std::shared_ptr<StatPair> mCountBasicForks;
	std::shared_ptr<StatPair> mCountCallForks;
	std::shared_ptr<StatPair> mCountMessageForks;
	std::shared_ptr<StatPair> mCountMessageProxyForks;
};

class ModuleRouter : public Module,
                     public ModuleToolbox,
                     public ForkContextListener,
                     public std::enable_shared_from_this<ModuleRouter> {
public:
	ModuleRouter(Agent* agent);
	~ModuleRouter();

	void onDeclare(GenericStruct* mc) override;
	void onLoad(const GenericStruct* mc) override;
	void onUnload() override;
	void onRequest(std::shared_ptr<RequestSipEvent>& ev) override;
	void onResponse(std::shared_ptr<ResponseSipEvent>& ev) override;
	void onForkContextFinished(const std::shared_ptr<ForkContext>& ctx) override;

	void sendReply(std::shared_ptr<RequestSipEvent>& ev,
	               int code,
	               const char* reason,
	               int warn_code = 0,
	               const char* warning = nullptr);
	void routeRequest(std::shared_ptr<RequestSipEvent>& ev, const std::shared_ptr<Record>& aor, const url_t* sipUri);
	void onContactRegistered(const std::shared_ptr<OnContactRegisteredListener>& listener,
	                         const std::string& uid,
	                         const std::shared_ptr<Record>& aor,
	                         const std::string& sipKey);

	const std::string& getFallbackRoute() const;
	const url_t* getFallbackRouteParsed() const;
	bool isFallbackToParentDomainEnabled() const;
	bool isDomainRegistrationAllowed() const;
	bool isManagedDomain(const url_t* url);
	const std::shared_ptr<SipBooleanExpression>& getFallbackRouteFilter() const;

	RouterStats mStats;

protected:
	using ForkMapElem = std::shared_ptr<ForkContext>;
	using ForkMap = std::multimap<std::string, ForkMapElem>;
	using ForkRefList = std::vector<ForkMapElem>;

	virtual void dispatch(const std::shared_ptr<ForkContext> context,
	                      const std::shared_ptr<ExtendedContact>& contact,
	                      const std::string& targetUris);
	std::string routingKey(const url_t* sipUri);
	std::vector<std::string> split(const char* data, const char* delim);
	ForkRefList getLateForks(const std::string& key) const noexcept;
	unsigned countLateForks(const std::string& key) const noexcept;

private:
	void restoreForksFromDatabase();

	std::unique_ptr<ModuleRouterPrivAttr> mAttr{};

	friend struct ModuleRouterPrivAttr;
};

} // namespace flexisip
