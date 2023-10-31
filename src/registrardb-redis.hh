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

#include <string>
#include <variant>
#include <vector>

#include "compat/hiredis/async.h"
#include "compat/hiredis/hiredis.h"

#include <sofia-sip/nta.h>
#include <sofia-sip/sip.h>

#include "flexisip/sofia-wrapper/su-root.hh"

#include "agent.hh"
#include "eventlogs/writers/event-log-writer.hh"
#include "libhiredis-wrapper/redis-async-session.hh"
#include "libhiredis-wrapper/redis-reply.hh"
#include "recordserializer.hh"
#include "registrar/binding-parameters.hh"
#include "registrar/change-set.hh"
#include "registrar/extended-contact.hh"
#include "registrar/registrar-db.hh"
#include "timed-redis-command.hh"

namespace flexisip {

namespace redis::auth {

class None {};
class Legacy {
public:
	std::string password;
};
class ACL {
public:
	std::string user;
	std::string password;
};

} // namespace redis::auth

struct RedisParameters {
	std::string domain{};
	std::variant<redis::auth::None, redis::auth::Legacy, redis::auth::ACL> auth{};
	int port = 0;
	int timeout = 0;
	std::chrono::seconds mSlaveCheckTimeout{0};
	bool useSlavesAsBackup = true;
};

/**
 * @brief The RedisHost struct, which is used to store redis slave description.
 */
struct RedisHost {
	RedisHost(int id, const std::string& address, unsigned short port, const std::string& state)
	    : id(id), address(address), port(port), state(state) {
	}

	RedisHost() {
		// invalid host
		id = -1;
	}

	inline bool operator==(const RedisHost& r) {
		return id == r.id && address == r.address && port == r.port && state == r.state;
	}

	/**
	 * @brief parseSlave this class method will parse a line from Redis where a slave information is expected.
	 *
	 * If the parsing goes well, the returned RedisHost will have the id field set to the one passed as argument,
	 * otherwise -1.
	 * @param slaveLine the Redis answer line where a slave is defined. Format is "host,port,state"
	 * @param id an ID to give to this slave, usually its number.
	 * @return A RedisHost with a valid ID or -1 if the parsing failed.
	 */
	static RedisHost parseSlave(const std::string& slaveLine, int id);
	int id;
	std::string address;
	unsigned short port;
	std::string state;
};

/******
 * RedisRegisterContext helper class
 */
class RegistrarDbRedisAsync;
struct RedisRegisterContext;

typedef void(forwardFn)(redisAsyncContext*, redisReply*, RedisRegisterContext*);

struct RedisRegisterContext {
	RegistrarDbRedisAsync* self = nullptr;
	std::shared_ptr<ContactUpdateListener> listener;
	std::shared_ptr<Record> mRecord;
	ChangeSet mChangeSet{};
	unsigned long token = 0;
	su_timer_t* mRetryTimer = nullptr;
	int mRetryCount = 0;
	bool mUpdateExpire = false;
	MsgSip mMsg;
	BindingParameters mBindingParameters;
	std::string mUniqueIdToFetch;

	template <typename T>
	RedisRegisterContext(RegistrarDbRedisAsync* s, T&& url, const std::shared_ptr<ContactUpdateListener>& listener)
	    : self(s), listener(listener), mRecord(std::make_shared<Record>(std::forward<T>(url))) {
	}
	RedisRegisterContext(RegistrarDbRedisAsync* s,
	                     const MsgSip& msg,
	                     const BindingParameters& params,
	                     const std::shared_ptr<ContactUpdateListener>& listener)
	    : self(s), listener(listener), mRecord(std::make_shared<Record>(SipUri(msg.getSip()->sip_from->a_url))),
	      mMsg(const_cast<MsgSip&>(msg).getMsg()), // Forcefully take a ref, instead of cloning
	      mBindingParameters(params) {
		// Note that MsgSip copy constructor is not invoked in order to avoid a deep copy.
		// Instead, mMsg just takes a ref on the underlying sofia-sip msg_t.
	}
};

class RegistrarDbRedisAsync : public RegistrarDb {
public:
	friend class redis::RedisCommandTimer;

	RegistrarDbRedisAsync(Agent* agent, RedisParameters params);
	RegistrarDbRedisAsync(const std::shared_ptr<sofiasip::SuRoot>& root,
	                      RecordSerializer* serializer,
	                      RedisParameters params,
	                      Agent* = nullptr);
	~RegistrarDbRedisAsync() override;

	void fetchExpiringContacts(time_t startTimestamp,
	                           float threshold,
	                           std::function<void(std::vector<ExtendedContact>&&)>&& callback) const override;

	std::optional<std::tuple<redis::async::Session::Ready&, redisAsyncContext&>> connect();
	void asyncDisconnect();
	void forceDisconnect();

protected:
	void doBind(const MsgSip& msg,
	            const BindingParameters& parameters,
	            const std::shared_ptr<ContactUpdateListener>& listener) override;
	void doClear(const MsgSip& msg, const std::shared_ptr<ContactUpdateListener>& listener) override;
	void doFetch(const SipUri& url, const std::shared_ptr<ContactUpdateListener>& listener) override;
	void doFetchInstance(const SipUri& url,
	                     const std::string& uniqueId,
	                     const std::shared_ptr<ContactUpdateListener>& listener) override;
	bool subscribe(const std::string& topic, std::weak_ptr<ContactRegisteredListener>&& listener) override;
	void unsubscribe(const std::string& topic, const std::shared_ptr<ContactRegisteredListener>& listener) override;
	void publish(const std::string& topic, const std::string& uid) override;

private:
	friend class RegistrarDb;

	static void sSubscribeConnectCallback(const redisAsyncContext* c, int status);
	static void sSubscribeDisconnectCallback(const redisAsyncContext* c, int status);
	static void sPublishCallback(redisAsyncContext* c, void* r, void* privdata);
	static void sKeyExpirationPublishCallback(redisAsyncContext* c, void* r, void* data);
	static void sBindRetry(void* unused, su_timer_t* t, void* ud);
	bool isConnected();
	void setWritable(bool value);
	redis::async::Session::Ready* tryGetCommandSession();

	void serializeAndSendToRedis(RedisRegisterContext&, redis::async::Session::CommandCallback&&);
	void subscribeTopic(const std::string& topic);
	void subscribeAll();
	void subscribeToKeyExpiration();
	static std::vector<std::unique_ptr<ExtendedContact>> parseContacts(const redis::reply::ArrayOfPairs&);

	/* callbacks */
	void handleAuthReply(const redisReply* reply);
	void handleBind(redis::async::Reply, std::unique_ptr<RedisRegisterContext>&&);
	void handleClear(redis::async::Reply, const RedisRegisterContext&);
	void handleFetch(redis::async::Reply, const RedisRegisterContext&);

	/**
	 * This callback is called when the Redis instance answered our "INFO replication" message.
	 * We parse the response to determine if we are connected to the master Redis instance or
	 * a slave, and we react accordingly.
	 * @param str Redis answer
	 */
	void handleReplicationInfoReply(const redis::reply::String& str);
	void onSubscribeConnect(const redisAsyncContext* c, int status);
	void onSubscribeDisconnect(const redisAsyncContext* c, int status);

	/* replication */
	void getReplicationInfo(redis::async::Session::Ready&);
	void updateSlavesList(const std::map<std::string, std::string>& redisReply);
	void tryReconnect();

	/* static handlers */
	// static void sHandleAorGetReply(struct redisAsyncContext *, void *r, void *privdata);
	static void sHandleAuthReply(redisAsyncContext* ac, void* r, void* privdata);
	static void sHandleSubcommandReply(redisAsyncContext*, redisReply* reply, std::string* cmd);

	/**
	 * This callback is called periodically to check if the current REDIS connection is valid
	 */
	void onHandleInfoTimer();

	/**
	 * Callback use to add space between RegistrarDbRedisAsync::tryReconnect calls
	 */
	void onTryReconnectTimer();

	redis::async::Session mCommandSession{};
	// From https://redis.io/commands/subscribe/
	// "Once the client enters the subscribed state it is not supposed to issue any other commands, except for
	// additional SUBSCRIBE, SSUBSCRIBE, PSUBSCRIBE, UNSUBSCRIBE, SUNSUBSCRIBE, PUNSUBSCRIBE, PING, RESET and QUIT
	// commands"
	redisAsyncContext* mSubscriptionSession{nullptr};
	RecordSerializer* mSerializer;
	RedisParameters mParams{};
	RedisParameters mLastActiveParams{};
	std::shared_ptr<sofiasip::SuRoot> mRoot{};
	std::vector<RedisHost> mSlaves{};
	decltype(mSlaves)::const_iterator mCurSlave = mSlaves.cend();
	std::unique_ptr<sofiasip::Timer> mReplicationTimer{nullptr};
	std::unique_ptr<sofiasip::Timer> mReconnectTimer{nullptr};
	std::chrono::system_clock::time_point mLastReconnectRotation;
	redis::RedisCommandTimer mTimedCommand{};
};

} // namespace flexisip
