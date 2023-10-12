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

#pragma once

#include <iostream>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include <belle-sip/belle-sip.h>
#if ENABLE_SOCI
#include <soci/soci.h>
#endif

#include "belle-sip-using.hh"
#include "bellesip-signaling-exception.hh"
#include "flexisip/configmanager.hh"
#include "observers/presence-info-observer.hh"
#include "presence/presentity/map-presentity-manager.hh"
#include "presence/presentity/presentity-manager.hh"
#include "service-server.hh"
#include "utils/thread/thread-pool.hh"

namespace flexisip {

class Subscription;
class PresentityPresenceInformation;

class PresenceServer : public ServiceServer {
public:
	// Used to declare the service configuration
	class Init {
	public:
		explicit Init(GenericStruct& configRoot);
	};

	explicit PresenceServer(const std::shared_ptr<sofiasip::SuRoot>& root);
	~PresenceServer() override;
	void _init() override;
	void _run() override;
	void _stop() override;
	belle_sip_main_loop_t* getBelleSipMainLoop();
	void enableLongTermPresence();

	static unsigned int sLastActivityRetentionMs;

private:
	template <typename T, typename BelleSipObjectT>
	static void setSubscription(BelleSipObjectT* obj, const std::shared_ptr<T>& sub) {
		belle_sip_object_data_set(BELLE_SIP_OBJECT(obj), sSubscriptionDataTag, new std::shared_ptr<Subscription>{sub},
		                          [](void* data) { delete static_cast<std::shared_ptr<Subscription>*>(data); });
	}

	template <typename BelleSipObjectT>
	static std::shared_ptr<Subscription> getSubscription(const BelleSipObjectT* obj) {
		auto data = belle_sip_object_data_get(BELLE_SIP_OBJECT(obj), sSubscriptionDataTag);
		return data ? *static_cast<std::shared_ptr<Subscription>*>(data) : nullptr;
	}

	// belle sip cbs
	static void processDialogTerminated(PresenceServer* thiz, const belle_sip_dialog_terminated_event_t* event);
	static void processIoError(PresenceServer* thiz, const belle_sip_io_error_event_t* event);
	static void processRequestEvent(PresenceServer* thiz, const belle_sip_request_event_t* event);
	static void processResponseEvent(PresenceServer* thiz, const belle_sip_response_event_t* event);
	static void processTimeout(PresenceServer* thiz, const belle_sip_timeout_event_t* event);
	static void processTransactionTerminated(PresenceServer* thiz,
	                                         const belle_sip_transaction_terminated_event_t* event);
	void processPublishRequestEvent(const belle_sip_request_event_t* event);
	void processSubscribeRequestEvent(const belle_sip_request_event_t* event);

	void removeSubscription(std::shared_ptr<Subscription>& identity);

	static Init sStaticInit;
	belle_sip_stack_t* mStack;
	belle_sip_provider_t* mProvider;
	belle_sip_listener_t* mListener;
	int mDefaultExpires;
	std::string mBypass;
	std::string mRequest;
#if ENABLE_SOCI
	soci::connection_pool* mConnPool = nullptr;
#endif
	std::unique_ptr<ThreadPool> mThreadPool{};
	bool mEnabled;
	size_t mMaxPresenceInfoNotifiedAtATime;
	std::unique_ptr<PresentityManager> mPresentityManager{};

	static constexpr const char* sSubscriptionDataTag = "subscription";
};

} // namespace flexisip
