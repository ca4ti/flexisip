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

#include "async.h"
#include "registrar/registrar-db.hh"
#include "registrardb-redis.hh"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include <sofia-sip/sip_protos.h>

#include "flexisip/configmanager.hh"
#include "flexisip/registrar/registar-listeners.hh"

#include "compat/hiredis/hiredis.h"
#include "libhiredis-wrapper/redis-args-packer.hh"
#include "libhiredis-wrapper/redis-async-context.hh"
#include "libhiredis-wrapper/redis-async-script.hh"
#include "libhiredis-wrapper/redis-reply.hh"
#include "recordserializer.hh"
#include "registrar/exceptions.hh"
#include "registrar/extended-contact.hh"
#include "registrardb-redis-sofia-event.h"
#include "timed-redis-command.hh"
#include "utils/variant-utils.hh"

/* The timeout to retry a bind request after encountering a failure. It gives us a chance to reconnect to a new
 * master.*/
constexpr int redisRetryTimeoutMs = 5000;

using namespace std;

namespace flexisip {
using namespace redis;
using namespace redis::async;
using namespace redis::reply;
using Reply = reply::Reply;

namespace {

const Script FETCH_EXPIRING_CONTACTS_SCRIPT{
#include "fetch-expiring-contacts.lua.hh"
    , // ❯ sed -n '/R"lua(/,/)lua"/p' fetch-expiring-contacts.lua.hh | sed 's/R"lua(//' | head -n-1 | sha1sum
    "8f26674ebf2a65c4eee45d2ae9b98c121cf6ff43"};

} // namespace

/******
 * RegistrarDbRedisAsync class
 */

RegistrarDbRedisAsync::RegistrarDbRedisAsync(Agent* ag, RedisParameters params)
    : RegistrarDbRedisAsync(ag->getRoot(), RecordSerializer::get(), params, ag) {
}

RegistrarDbRedisAsync::RegistrarDbRedisAsync(const std::shared_ptr<sofiasip::SuRoot>& root,
                                             RecordSerializer* serializer,
                                             RedisParameters params,
                                             Agent* ag)
    : RegistrarDb{ag}, mSerializer{serializer}, mParams{params}, mRoot{root} {
	const auto reconnectOnError = [this](int status) {
		if (status != REDIS_OK) tryReconnect();
	};
	mCommandSession.onConnect(reconnectOnError);
	mCommandSession.onDisconnect(reconnectOnError);
}

RegistrarDbRedisAsync::~RegistrarDbRedisAsync() {
	if (mSubscriptionSession) {
		redisAsyncDisconnect(mSubscriptionSession);
	}
}

void RegistrarDbRedisAsync::onSubscribeDisconnect(const redisAsyncContext* c, int status) {
	if (mSubscriptionSession != nullptr && mSubscriptionSession != c) {
		LOGE("Redis subscribe context %p disconnected, but current context is %p", c, mSubscriptionSession);
		return;
	}

	mSubscriptionSession = nullptr;
	LOGD("Disconnected subscribe context %p...", c);
	if (status != REDIS_OK) {
		LOGE("Redis disconnection message: %s", c->errstr);
		tryReconnect();
		return;
	}
}

void RegistrarDbRedisAsync::onSubscribeConnect(const redisAsyncContext* c, int status) {
	if (status != REDIS_OK) {
		LOGE("Couldn't connect for subscribe channel to redis: %s", c->errstr);
		if (mSubscriptionSession != nullptr && mSubscriptionSession != c) {
			LOGE("Redis subscribe context %p connection failed, but current subscribe context is %p", c,
			     mSubscriptionSession);
			return;
		}
		mSubscriptionSession = nullptr;
		tryReconnect();
		return;
	}
	LOGD("REDIS Connection done for subscribe channel %p", c);
	if (!mContactListenersMap.empty()) {
		LOGD("Now re-subscribing all topics we had before being disconnected.");
		subscribeAll();
	}
	subscribeToKeyExpiration();
}

bool RegistrarDbRedisAsync::isConnected() {
	return mCommandSession.isConnected();
}

void RegistrarDbRedisAsync::setWritable(bool value) {
	SLOGD << "Switch Redis RegistrarDB backend 'writable' flag [ " << mWritable << " -> " << value << " ]";
	mWritable = value;
	notifyStateListener();
}

namespace {

bool is_end_line_character(char c) {
	return c == '\r' || c == '\n';
}

/**
 * @brief parseKeyValue this functions parses a string contraining a list of key/value
 * separated by a delimiter, and for each key-value, another delimiter.
 * It converts the string to a map<string,string>.
 *
 * For instance:
 * <code>parseKeyValue("toto:tata\nfoo:bar", '\n', ':', '#')</code>
 * will give you:
 * <code>{ make_pair("toto","tata"), make_pair("foo", "bar") }</code>
 *
 * @param toParse the string to parse
 * @param delimiter the delimiter between key and value (default is ':')
 * @param comment a character which is a comment. Lines starting with this character
 * will be ignored.
 * @return a map<string,string> which contains the keys and values extracted (can be empty)
 */
map<string, string> parseKeyValue(const string& toParse,
                                  const char line_delim = '\n',
                                  const char delimiter = ':',
                                  const char comment = '#') {
	map<string, string> kvMap;
	istringstream values(toParse);

	for (string line; getline(values, line, line_delim);) {
		if (line.find(comment) == 0) continue; // section title

		// clear all non-UNIX end of line chars
		line.erase(remove_if(line.begin(), line.end(), is_end_line_character), line.end());

		size_t delim_pos = line.find(delimiter);
		if (delim_pos == line.npos || delim_pos == line.length()) {
			LOGW("Invalid line '%s' in key-value", line.c_str());
			continue;
		}

		const string key = line.substr(0, delim_pos);
		string value = line.substr(delim_pos + 1);

		kvMap[key] = value;
	}

	return kvMap;
}

auto logErrorReply(const ArgsPacker& cmd) {
	return [cmd = cmd.toString()](Session&, Reply reply) {
		if (auto* err = std::get_if<reply::Error>(&reply)) {
			SLOGW << "Redis subcommand failure [" << cmd << "]: " << *err;
		}
	};
}

} // namespace

RedisHost RedisHost::parseSlave(const string& slave, int id) {
	istringstream input(slave);
	vector<string> context;
	// a slave line has this format for redis < 2.8: "<host>,<port>,<state>"
	// for redis > 2.8 it is this format: "ip=<ip>,port=<port>,state=<state>,...(key)=(value)"

	// split the string with ',' into an array
	for (string token; getline(input, token, ',');)
		context.push_back(token);

	if (context.size() > 0 && (context.at(0).find('=') != string::npos)) {
		// we have found an "=" in one of the values: the format is post-Redis 2.8.
		// We have to parse is accordingly.
		auto m = parseKeyValue(slave, ',', '=');

		if (m.find("ip") != m.end() && m.find("port") != m.end() && m.find("state") != m.end()) {
			return RedisHost(id, m.at("ip"), atoi(m.at("port").c_str()), m.at("state"));
		} else {
			SLOGW << "Missing fields in the slaveline " << slave;
		}
	} else if (context.size() >= 3) {
		// Old-style slave format, use the context from the array directly
		return RedisHost(id, context[0],                           // host
		                 (unsigned short)atoi(context[1].c_str()), // port
		                 context[2]);                              // state
	} else {
		SLOGW << "Invalid host line: " << slave;
	}
	return RedisHost(); // invalid host
}

void RegistrarDbRedisAsync::updateSlavesList(const map<string, string>& redisReply) {
	decltype(mSlaves) newSlaves;

	try {
		int slaveCount = atoi(redisReply.at("connected_slaves").c_str());
		for (int i = 0; i < slaveCount; i++) {
			std::stringstream sstm;
			sstm << "slave" << i;
			string slaveName = sstm.str();

			if (redisReply.find(slaveName) != redisReply.end()) {

				RedisHost host = RedisHost::parseSlave(redisReply.at(slaveName), i);
				if (host.id != -1) {
					// only tell if a new host was found
					if (std::find(mSlaves.begin(), mSlaves.end(), host) == mSlaves.end()) {
						LOGD("Replication: Adding host %d %s:%d state:%s", host.id, host.address.c_str(), host.port,
						     host.state.c_str());
					}
					newSlaves.push_back(host);
				}
			}
		}
	} catch (const out_of_range&) {
	}

	for (const auto& oldSlave : mSlaves) {
		if (find(newSlaves.begin(), newSlaves.end(), oldSlave) == newSlaves.end()) {
			LOGD("Replication: Removing host %d %s:%d previous state:%s", oldSlave.id, oldSlave.address.c_str(),
			     oldSlave.port, oldSlave.state.c_str());
		}
	}

	// replace the slaves array
	mSlaves = std::move(newSlaves);
	mCurSlave = mSlaves.cend();
}

void RegistrarDbRedisAsync::onTryReconnectTimer() {
	tryReconnect();
	mReconnectTimer.reset(nullptr);
}

void RegistrarDbRedisAsync::tryReconnect() {
	if (isConnected()) {
		return;
	}

	if (chrono::system_clock::now() - mLastReconnectRotation < 1s) {
		if (!mReconnectTimer.get()) {
			mReconnectTimer = make_unique<sofiasip::Timer>(mRoot, 1s);
			mReconnectTimer->set([this]() { onTryReconnectTimer(); });
		}
		return;
	}

	// First we try to reconnect using the last active connection
	if (mCurSlave == mSlaves.cend()) {
		// We need to restore mLastActiveParams if we already tried all slaves without success to try the last master
		// again.
		mParams = mLastActiveParams;
		if ((mCurSlave = mSlaves.cbegin()) == mSlaves.cend()) {
			// If there is no slaves, this is already a full rotation.
			mLastReconnectRotation = std::chrono::system_clock::now();
		}
		LOGW("Trying to reconnect to last active connection at %s:%d", mParams.domain.c_str(), mParams.port);
		connect();
		return;
	}

	// If last active connection still fail
	// we can try one of the previously determined slaves
	if (mCurSlave != mSlaves.cend()) {
		LOGW("Connection failed or lost to %s:%d, trying a known slave %d at %s:%d", mParams.domain.c_str(),
		     mParams.port, mCurSlave->id, mCurSlave->address.c_str(), mCurSlave->port);

		mParams.domain = mCurSlave->address;
		mParams.port = mCurSlave->port;
		if (++mCurSlave == mSlaves.cend()) {
			mLastReconnectRotation = std::chrono::system_clock::now();
		}
		connect();

	} else {
		LOGW("No slave to try, giving up.");
	}
}

void RegistrarDbRedisAsync::handleReplicationInfoReply(const reply::String& reply) {
	SLOGD << "Redis replication information received";
	auto replyMap = parseKeyValue(std::string(reply));
	if (replyMap.find("role") != replyMap.end()) {
		string role = replyMap["role"];
		if (role == "master") {
			// We are speaking to the master, set the DB as writable and update the list of slaves
			SLOGD << "Redis server is a master";
			setWritable(true);
			if (mParams.useSlavesAsBackup) {
				updateSlavesList(replyMap);
			}
		} else if (role == "slave") {
			// woops, we are connected to a slave. We should go to the master
			string masterAddress = replyMap["master_host"];
			int masterPort = atoi(replyMap["master_port"].c_str());
			string masterStatus = replyMap["master_link_status"];

			LOGW("Our redis instance is a slave of %s:%d", masterAddress.c_str(), masterPort);
			if (masterStatus == "up") {
				SLOGW << "Master is up, will attempt to connect to the master at " << masterAddress << ":"
				      << masterPort;

				mParams.domain = masterAddress;
				mParams.port = masterPort;

				// disconnect and reconnect immediately, dropping the previous context
				forceDisconnect();
				connect();
			} else {
				SLOGW << "Master is " << masterStatus
				      << " but not up, wait for next periodic check to decide to connect.";
			}
		} else {
			SLOGW << "Unknown role '" << role << "'";
		}
		if (!mReplicationTimer.get()) {
			SLOGD << "Creating replication timer with delay of " << mParams.mSlaveCheckTimeout.count() << "s";
			mReplicationTimer = make_unique<sofiasip::Timer>(mRoot, mParams.mSlaveCheckTimeout);
			mReplicationTimer->run([this]() { onHandleInfoTimer(); });
		}
	} else {
		SLOGW << "Invalid INFO reply: no role specified";
	}
}

void RegistrarDbRedisAsync::handleAuthReply(const redisReply* reply) {
	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Couldn't authenticate with Redis server");
		asyncDisconnect();
		return;
	}

	Match(mCommandSession.getState())
	    .against([this](Session::Ready& session) { getReplicationInfo(session); },
	             [](const auto& unexpected) {
		             // Somehow happened in production before the hiredis wrapper was written
		             SLOGE << "Receiving success response to Redis AUTH request but we are no longer connected. This "
		                      "should never happen! Aborting replication info fetch! Unexpected session state: "
		                   << unexpected;
	             });
}

void RegistrarDbRedisAsync::getReplicationInfo(Session::Ready& cmdSession) {
	SLOGD << "Collecting replication information";
	cmdSession.command({"INFO", "replication"}, [this](const Session&, Reply reply) {
		Match(reply).against(
		    [this](const reply::String& reply) { handleReplicationInfoReply(reply); },
		    [](const auto& unexpected) { SLOGE << "Unexpected reply to INFO command: " << unexpected; });
	});

	if (mSubscriptionSession) {
		// Workaround for issue https://github.com/redis/hiredis/issues/396
		redisAsyncCommand(mSubscriptionSession, sPublishCallback, nullptr, "SUBSCRIBE %s", "FLEXISIP");
	}
}

optional<tuple<Session::Ready&, redisAsyncContext&>> RegistrarDbRedisAsync::connect() {
	SLOGD << "Connecting to Redis server tcp://" << mParams.domain << ":" << mParams.port;
	SLOGD << "Creating main Redis connection";
	Session::Ready* cmdSession = nullptr;
	{
		auto& state = mCommandSession.connect(mRoot->getCPtr(), mParams.domain.c_str(), mParams.port);
		if ((cmdSession = std::get_if<Session::Ready>(&state)) == nullptr) return {};
	}

	SLOGD << "Creating subscription Redis connection";
	mSubscriptionSession = redisAsyncConnect(mParams.domain.c_str(), mParams.port);
	mSubscriptionSession->data = this;
	if (mSubscriptionSession->err) {
		SLOGE << "Redis Connection error: " << mSubscriptionSession->errstr;
		redisAsyncFree(mSubscriptionSession);
		mSubscriptionSession = nullptr;
		return {};
	}

#ifndef WITHOUT_HIREDIS_CONNECT_CALLBACK
	redisAsyncSetConnectCallback(mSubscriptionSession, sSubscribeConnectCallback);
#endif

	redisAsyncSetDisconnectCallback(mSubscriptionSession, sSubscribeDisconnectCallback);

	if (REDIS_OK != redisSofiaAttach(mSubscriptionSession, mRoot->getCPtr())) {
		LOGE("Redis Connection error - %p", mSubscriptionSession);
		redisAsyncDisconnect(mSubscriptionSession);
		mSubscriptionSession = nullptr;
		return {};
	}

	// Clear any pending timing context remaining from a previous connection
	mTimedCommand = {};

	const auto authenticate = [this, cmdSession](const redis::ArgsPacker& args) {
		cmdSession->command(args, [this](const Session&, Reply reply) {
			if (auto* err = std::get_if<reply::Error>(&reply)) {
				SLOGE << "Couldn't authenticate with Redis server: " << *err;
				asyncDisconnect();
				return;
			}
			SLOGD << "Redis authentication succeeded. Reply: " << StreamableVariant(reply);
			if (auto* session = std::get_if<Session::Ready>(&mCommandSession.getState())) {
				getReplicationInfo(*session);
			}
		});
		mTimedCommand.send(mSubscriptionSession, sHandleAuthReply, this, args);
	};

	Match(mParams.auth)
	    .against([this, cmdSession](redis::auth::None) { getReplicationInfo(*cmdSession); },
	             [&authenticate](redis::auth::Legacy legacy) {
		             authenticate({"AUTH", legacy.password});
	             },
	             [&authenticate](redis::auth::ACL acl) {
		             authenticate({"AUTH", acl.user, acl.password});
	             });

	mLastActiveParams = mParams;
	return {{*cmdSession, *mSubscriptionSession}};
}

void RegistrarDbRedisAsync::asyncDisconnect() {
	setWritable(false);
	mCommandSession.disconnect();
	if (mSubscriptionSession) {
		// Workaround for issue https://github.com/redis/hiredis/issues/396
		// Fixed in hiredis 0.14.0
		redisAsyncCommand(mSubscriptionSession, nullptr, nullptr, "UNSUBSCRIBE %s", "FLEXISIP");
		redisAsyncDisconnect(mSubscriptionSession);
		mSubscriptionSession = nullptr;
	}
	SLOGD << "Redis server disconnecting...";
}
void RegistrarDbRedisAsync::forceDisconnect() {
	setWritable(false);
	mCommandSession.getState() = Session::Disconnected();
	if (mSubscriptionSession) {
		redisAsyncFree(mSubscriptionSession);
		mSubscriptionSession = nullptr;
	}
	SLOGD << "Redis server force-disconnected";
}

// This function is invoked after a redis disconnection on the subscribe channel, so that all topics we are interested
// in are re-subscribed
void RegistrarDbRedisAsync::subscribeAll() {
	set<string> topics;
	for (auto it = mContactListenersMap.begin(); it != mContactListenersMap.end(); ++it)
		topics.insert(it->first);
	for (const auto& topic : topics)
		subscribeTopic(topic);
}

void RegistrarDbRedisAsync::subscribeToKeyExpiration() {
	LOGD("Subscribing to key expiration");
	if (mSubscriptionSession == nullptr) {
		LOGE("RegistrarDbRedisAsync::subscribeToKeyExpiration(): no context !");
		return;
	}
	redisAsyncCommand(mSubscriptionSession, sKeyExpirationPublishCallback, nullptr, "SUBSCRIBE __keyevent@0__:expired");
}

void RegistrarDbRedisAsync::subscribeTopic(const string& topic) {
	LOGD("Sending SUBSCRIBE command to redis for topic '%s'", topic.c_str());
	if (mSubscriptionSession == nullptr) {
		LOGE("RegistrarDbRedisAsync::subscribeTopic(): no context !");
		return;
	}
	redisAsyncCommand(mSubscriptionSession, sPublishCallback, nullptr, "SUBSCRIBE %s", topic.c_str());
}

/*TODO: the listener should be also used to report when the subscription is active.
 * Indeed if we send a push notification to a device while REDIS has not yet confirmed the subscription, we will not do
 * anything when receiving the REGISTER from the device. The router module should wait confirmation that subscription is
 * active before injecting the forked request to the module chain.*/
bool RegistrarDbRedisAsync::subscribe(const string& topic, std::weak_ptr<ContactRegisteredListener>&& listener) {
	auto shouldSubscribe = RegistrarDb::subscribe(topic, std::move(listener));
	if (shouldSubscribe) {
		subscribeTopic(topic);
		return true;
	}
	return false;
}

void RegistrarDbRedisAsync::unsubscribe(const string& topic, const shared_ptr<ContactRegisteredListener>& listener) {
	RegistrarDb::unsubscribe(topic, listener);
	if (mContactListenersMap.count(topic) == 0) {
		SLOGD << "Sending UNSUBSCRIBE command to Redis for topic '" << topic << "'";
		redisAsyncCommand(mSubscriptionSession, nullptr, nullptr, "UNSUBSCRIBE %s", topic.c_str());
	}
}

void RegistrarDbRedisAsync::publish(const string& topic, const string& uid) {
	SLOGD << "Publish topic = " << topic << ", uid = " << uid;
	Match(mCommandSession.getState())
	    .against(
	        [&topic, &uid](Session::Ready& ready) {
		        ready.command({"PUBLISH", topic, uid}, {});
	        },
	        [](auto&) { SLOGE << "RegistrarDbRedisAsync::publish(): no context !"; });
}

/* Static functions that are used as callbacks to redisAsync API */

#ifndef WITHOUT_HIREDIS_CONNECT_CALLBACK
void RegistrarDbRedisAsync::sSubscribeConnectCallback(const redisAsyncContext* c, int status) {
	RegistrarDbRedisAsync* zis = (RegistrarDbRedisAsync*)c->data;
	if (zis) {
		zis->onSubscribeConnect(c, status);
	}
}
#endif

void RegistrarDbRedisAsync::sSubscribeDisconnectCallback(const redisAsyncContext* c, int status) {
	RegistrarDbRedisAsync* zis = (RegistrarDbRedisAsync*)c->data;
	if (zis) {
		zis->onSubscribeDisconnect(c, status);
	}
}

void RegistrarDbRedisAsync::sPublishCallback(redisAsyncContext* c, void* r, [[maybe_unused]] void* privcontext) {
	const auto* reply = static_cast<redisReply*>(r);
	if (reply == nullptr) return;

	if (reply->type == REDIS_REPLY_ARRAY) {
		const auto& messageType = reply->element[0]->str;
		const auto& channel = reply->element[1]->str;
		if (strcasecmp(messageType, "message") == 0) {
			const auto& message = reply->element[2]->str;
			SLOGD << "Publish array received: [" << messageType << ", " << channel << ", " << message << "]";
			auto* zis = static_cast<RegistrarDbRedisAsync*>(c->data);
			if (zis) {
				zis->notifyContactListener(reply->element[1]->str, reply->element[2]->str);
			}
		} else {
			const auto& nSubscriptions = reply->element[2]->integer;
			SLOGD << "'" << messageType << "' request on '" << channel << "' channel succeeded. " << nSubscriptions
			      << " actual subscriptions";
		}
	}
}

void RegistrarDbRedisAsync::sKeyExpirationPublishCallback(redisAsyncContext* c, void* r, void*) {
	redisReply* reply = reinterpret_cast<redisReply*>(r);
	if (!reply) return;

	if (reply->type == REDIS_REPLY_ARRAY) {
		if (reply->element[2]->str != nullptr) {
			RegistrarDbRedisAsync* zis = reinterpret_cast<RegistrarDbRedisAsync*>(c->data);
			if (zis) {
				string prefix = "fs:";
				string key = reply->element[2]->str;
				if (key.substr(0, prefix.size()) == prefix) key = key.substr(prefix.size());
				zis->notifyContactListener(key, "");
			}
		}
	}
}

void RegistrarDbRedisAsync::sHandleSubcommandReply(redisAsyncContext*, redisReply* reply, std::string* cmd) {
	auto redisError = reply ? (reply->type == REDIS_REPLY_ERROR ? reply->str : nullptr) : "empty response";
	if (redisError) {
		SLOGW << "Redis subcommand failure [" << *cmd << "]: " << redisError;
	}
	delete cmd;
};

void RegistrarDbRedisAsync::onHandleInfoTimer() {
	if (auto* session = std::get_if<Session::Ready>(&mCommandSession.getState())) {
		SLOGI << "Launching periodic INFO query on REDIS";
		getReplicationInfo(*session);
	}
}

void RegistrarDbRedisAsync::sHandleAuthReply([[maybe_unused]] redisAsyncContext* ac, void* r, void* privcontext) {
	RegistrarDbRedisAsync* zis = (RegistrarDbRedisAsync*)privcontext;
	if (zis) {
		zis->handleAuthReply((const redisReply*)r);
	}
}

void RegistrarDbRedisAsync::serializeAndSendToRedis(RedisRegisterContext& context,
                                                    redis::async::Session::CommandCallback&& forwardedCb) {
	Session::Ready* cmdSession = nullptr;
	{
		auto& state = mCommandSession.getState();
		if ((cmdSession = std::get_if<Session::Ready>(&state)) == nullptr) {
			if (context.listener) context.listener->onError();
			return;
		}
	}

	int setCount = 0;
	int delCount = 0;
	string key = string("fs:") + context.mRecord->getKey();

	/* Start a REDIS transaction */
	cmdSession->command({"MULTI"}, {});

	/* First delete contacts that need to be deleted */
	if (!context.mChangeSet.mDelete.empty()) {
		redis::ArgsPacker hDelArgs("HDEL", key);
		for (const auto& ec : context.mChangeSet.mDelete) {
			hDelArgs.addFieldName(ec->mKey);
			delCount++;
		}
		cmdSession->command(hDelArgs, logErrorReply(hDelArgs));
		SLOGD << hDelArgs;
	}

	/* Update or set new ones */
	if (!context.mChangeSet.mUpsert.empty()) {
		redis::ArgsPacker hSetArgs("HMSET", key);
		for (const auto& ec : context.mChangeSet.mUpsert) {
			hSetArgs.addPair(ec->mKey, ec->serializeAsUrlEncodedParams());
			setCount++;
		}
		cmdSession->command(hSetArgs, logErrorReply(hSetArgs));
		SLOGD << hSetArgs;
	}

	LOGD("Binding %s [%i] contact sets, [%i] contacts removed.", key.c_str(), setCount, delCount);

	/* Set global expiration for the Record */
	redis::ArgsPacker expireAtCmd{"EXPIREAT", key, to_string(context.mRecord->latestExpire())};
	cmdSession->command(expireAtCmd, logErrorReply(expireAtCmd));

	/* Execute the transaction */
	cmdSession->command({"EXEC"}, std::move(forwardedCb));
}

/* Methods called by the callbacks */

void RegistrarDbRedisAsync::sBindRetry(void*, su_timer_t*, void* ud) {
	std::unique_ptr<RedisRegisterContext> context{static_cast<RedisRegisterContext*>(ud)};
	su_timer_destroy(context->mRetryTimer);
	context->mRetryTimer = nullptr;
	RegistrarDbRedisAsync* self = context->self;

	self->serializeAndSendToRedis(*context, [self, context = std::move(context)](Session&, Reply reply) mutable {
		self->handleBind(reply, std::move(context));
	});
	return;
}

void RegistrarDbRedisAsync::handleBind(Reply reply, std::unique_ptr<RedisRegisterContext>&& context) {
	Match(reply).against(
	    [&context](const reply::Array&) {
		    context->mRetryCount = 0;
		    if (context->listener) context->listener->onRecordFound(context->mRecord);
	    },
	    [&context, &agent = mAgent](const auto& reply) {
		    std::ostringstream log{};
		    log << "Error updating record fs:" << context->mRecord->getKey() << " [" << context->token
		        << "] hashmap in Redis. Reply: " << reply << "\n";
		    if (context->mRetryCount < 2) {
			    log << "Retrying in " << redisRetryTimeoutMs << "ms.";
			    context->mRetryCount += 1;
			    context->mRetryTimer = agent->createTimer(redisRetryTimeoutMs, sBindRetry, context.release(), false);
		    } else {
			    log << "Unrecoverable. No further attempt will be made.";
			    if (context->listener) context->listener->onError();
		    }
		    SLOGE << log.str();
	    });
}

Session::Ready* RegistrarDbRedisAsync::tryGetCommandSession() {
	auto& state = mCommandSession.getState();
	if (auto* ready = std::get_if<Session::Ready>(&state)) return ready;
	if (auto connected = connect()) {
		auto& [cmdSession, _] = *connected;
		return &cmdSession;
	}
	SLOGE << "Failed to connect to Redis server";
	return nullptr;
}

void RegistrarDbRedisAsync::doBind(const MsgSip& msg,
                                   const BindingParameters& parameters,
                                   const std::shared_ptr<ContactUpdateListener>& listener) {
	// - Fetch the record from redis
	// - update the Record from the message and binding parameters
	// - push the new record to redis by commiting changes to apply (set or remove).
	// - notify the onRecordFound().

	Session::Ready* cmdSession;
	if (!(cmdSession = tryGetCommandSession())) {
		if (listener) listener->onError();
		return;
	}

	auto context = std::make_unique<RedisRegisterContext>(this, msg, parameters, listener);
	mLocalRegExpire->update(context->mRecord);

	const auto& key = context->mRecord->getKey();
	cmdSession->command({"HGETALL", "fs:" + key}, [context = std::move(context), this](Session&, Reply reply) mutable {
		LOGD("Got current Record content for key [fs:%s].", context->mRecord->getKey().c_str());
		auto* array = std::get_if<reply::Array>(&reply);
		if (array == nullptr) {
			SLOGE << "Unexpected reply on Redis pre-bind fetch: " << StreamableVariant(reply);
			if (context->listener) context->listener->onError();
			return;
		}

		auto& contacts = context->mRecord->getExtendedContacts();
		auto& changeset = context->mChangeSet;
		// Parse the fetched reply into the Record object (context->mRecord)
		for (auto&& maybeExpired : parseContacts(array->pairwise())) {
			if (maybeExpired->isExpired()) {
				changeset.mDelete.emplace_back(std::move(maybeExpired));
			} else {
				contacts.emplace(std::move(maybeExpired));
			}
		}

		/* Now update the existing Record with new SIP REGISTER and binding parameters
		 * insertOrUpdateBinding() will do the job of contact comparison and invoke the onContactUpdated listener*/
		LOGD("Updating Record content for key [fs:%s] with new contact(s).", context->mRecord->getKey().c_str());
		try {
			changeset +=
			    context->mRecord->update(context->mMsg.getSip(), context->mBindingParameters, context->listener);
		} catch (const InvalidCSeq&) {
			if (context->listener) context->listener->onInvalid();
			return;
		}

		changeset += context->mRecord->applyMaxAor();

		/* now submit the changes triggered by the update operation to REDIS */
		SLOGD << "Sending updated content to REDIS for key [fs:" << context->mRecord->getKey() << "]: " << changeset;
		auto& ctxRef = *context;
		serializeAndSendToRedis(ctxRef, [this, context = std::move(context)](Session&, Reply reply) mutable {
			handleBind(reply, std::move(context));
		});
	});
}

void RegistrarDbRedisAsync::handleClear(Reply reply, const RedisRegisterContext& context) {
	const auto recordName = "fs:" + context.mRecord->getKey() + " [" + std::to_string(context.token) + "]";
	if (const auto* keysDeleted = std::get_if<reply::Integer>(&reply)) {
		if (*keysDeleted == 1) {
			SLOGD << "Record " << recordName << " successfully cleared";
			if (context.listener) context.listener->onRecordFound(context.mRecord);
			return;
		}
	}

	if (const auto* error = std::get_if<reply::Error>(&reply)) {
		if (error->find("READONLY") != string_view::npos) {
			SLOGW << "Redis couldn't DEL " << recordName << " because we're connected to a slave. Replying 480.";
			if (context.listener) context.listener->onRecordFound(nullptr);
			return;
		}
	}

	SLOGE << "Unexpected reply DELeting " << recordName << ": " << StreamableVariant(reply);
	if (context.listener) context.listener->onError();
}

vector<unique_ptr<ExtendedContact>> RegistrarDbRedisAsync::parseContacts(const reply::ArrayOfPairs& entries) {
	decltype(parseContacts(entries)) contacts{};
	contacts.reserve(entries.size());

	for (const auto [maybeKey, maybeContactStr] : entries) {
		SLOGD << "Parsing contact " << StreamableVariant(maybeKey) << " => " << StreamableVariant(maybeContactStr);
		const reply::String *key, *contactStr = nullptr;
		if (!(key = std::get_if<reply::String>(&maybeKey)) ||
		    !(contactStr = std::get_if<reply::String>(&maybeContactStr))) {
			SLOGE << "Unexpected key or contact type";
			continue;
		}

		auto maybeContact = make_unique<ExtendedContact>(key->data(), contactStr->data());
		if (maybeContact->mSipContact) {
			contacts.push_back(std::move(maybeContact));
		} else {
			LOGE("This contact could not be parsed.");
		}
	}

	return contacts;
}

void RegistrarDbRedisAsync::doClear(const MsgSip& msg, const shared_ptr<ContactUpdateListener>& listener) {
	Session::Ready* cmdSession;
	if (!(cmdSession = tryGetCommandSession())) {
		if (listener) listener->onError();
		return;
	}

	auto sip = msg.getSip();
	try {
		// Delete the AOR Hashmap using DEL
		// Once it is done, fetch all the contacts in the AOR and call the onRecordFound of the listener ?
		auto context = std::make_unique<RedisRegisterContext>(this, SipUri(sip->sip_from->a_url), listener);

		const string& key = context->mRecord->getKey();
		SLOGD << "Clearing fs:" << key << " [" << context->token << "]";
		mLocalRegExpire->remove(key);
		cmdSession->command({"DEL", "fs:" + key}, [context = std::move(context), this](Session&, Reply reply) {
			handleClear(reply, *context);
		});
	} catch (const sofiasip::InvalidUrlError& e) {
		SLOGE << "Invalid 'From' SIP URI [" << e.getUrl() << "]: " << e.getReason();
		listener->onInvalid();
	}
}

void RegistrarDbRedisAsync::handleFetch(redis::async::Reply reply, const RedisRegisterContext& context) {
	const auto& record = context.mRecord;
	const auto recordName = "fs:" + record->getKey() + " [" + std::to_string(context.token) + "]";
	const auto insertIfActive = [&record = *record](auto&& contact) {
		if (contact->isExpired()) return;

		try {
			record.insertOrUpdateBinding(std::move(contact), nullptr);
		} catch (const InvalidCSeq&) {
			// There can be a race condition on contact registration. If we get more REGISTERs (without sip instance)
			// before Redis responded to the first, then we issue multiple insertion commands resulting in duplicated
			// contacts, potentially with out-of-order CSeq. This situation will be resolved on the next bind (because
			// all those duplicated contacts will match the new contact, and all be deleted), so in the meantime, let's
			// just skip the duplicated contacts
			SLOGW << "Illegal state detected in the RegistrarDb. Skipping contact: "
			      << (contact ? contact->urlAsString() : "<moved out>");
		}
	};

	auto* listener = context.listener.get();
	Match(reply).against(
	    [&recordName, &insertIfActive, listener, &record, &context](const reply::Array& array) {
		    // This is the most common scenario: we want all contacts inside the record
		    const auto contacts = array.pairwise();
		    SLOGD << "GOT " << recordName << " --> " << contacts.size() << " contacts";
		    if (0 < contacts.size()) {
			    for (auto&& maybeExpired : parseContacts(contacts)) {
				    insertIfActive(maybeExpired);
			    }
			    if (listener) listener->onRecordFound(record);
		    } else {
			    // Anchor WKADREGMIGDELREC
			    // This is a workaround required in case of unregister (expire set to 0) because
			    // if there is only one entry, it will be deleted first so the fetch will come back empty
			    // and flexisip will answer 480 instead of 200.
			    if (listener) listener->onRecordFound(context.mBindingParameters.globalExpire == 0 ? record : nullptr);
		    }
	    },
	    [&context, &recordName, &insertIfActive, listener, &record](const reply::String& contact) {
		    // This is only when we want a contact matching a given gruu
		    const char* gruu = context.mUniqueIdToFetch.c_str();
		    if (!contact.empty()) {
			    SLOGD << "GOT " << recordName << " for gruu " << gruu << " --> " << contact;
			    insertIfActive(make_unique<ExtendedContact>(gruu, contact.data()));
			    if (listener) listener->onRecordFound(record);
		    } else {
			    SLOGD << "Contact matching gruu " << gruu << " in record " << recordName << " not found";
			    if (listener) listener->onRecordFound(nullptr);
		    }
	    },
	    [&recordName, listener](const auto& unexpected) {
		    SLOGE << "Unexpected Redis reply fetching " << recordName << ": " << unexpected;
		    if (listener) listener->onError();
	    });
}

void RegistrarDbRedisAsync::doFetch(const SipUri& url, const shared_ptr<ContactUpdateListener>& listener) {
	// fetch all the contacts in the AOR (HGETALL) and call the onRecordFound of the listener
	Session::Ready* cmdSession;
	if (!(cmdSession = tryGetCommandSession())) {
		if (listener) listener->onError();
		return;
	}

	auto context = std::make_unique<RedisRegisterContext>(this, url, listener);

	const auto& key = context->mRecord->getKey();
	SLOGD << "Fetching fs:" << key << " [" << context->token << "]";
	cmdSession->command({"HGETALL", "fs:" + key},
	                    [context = std::move(context), this](Session&, Reply reply) { handleFetch(reply, *context); });
}

void RegistrarDbRedisAsync::doFetchInstance(const SipUri& url,
                                            const string& uniqueId,
                                            const shared_ptr<ContactUpdateListener>& listener) {
	// fetch only the contact in the AOR (HGET) and call the onRecordFound of the listener
	Session::Ready* cmdSession;
	if (!(cmdSession = tryGetCommandSession())) {
		if (listener) listener->onError();
		return;
	}

	auto context = std::make_unique<RedisRegisterContext>(this, url, listener);
	context->mUniqueIdToFetch = uniqueId;

	const auto& recordKey = context->mRecord->getKey();
	SLOGD << "Fetching fs:" << recordKey << " [" << context->token << "] contact matching unique id " << recordKey;
	cmdSession->command({"HGET", "fs:" + recordKey, uniqueId},
	                    [context = std::move(context), this](Session&, Reply reply) { handleFetch(reply, *context); });
}

void RegistrarDbRedisAsync::fetchExpiringContacts(
    time_t startTimestamp, float threshold, std::function<void(std::vector<ExtendedContact>&&)>&& callback) const {
	const Session::Ready* cmdSession;
	if (!(cmdSession = std::get_if<Session::Ready>(&mCommandSession.getState()))) {
		SLOGW << "Redis session not ready to send commands. Cancelling fetchExpiringContacts operation";
		return;
	}

	FETCH_EXPIRING_CONTACTS_SCRIPT.call(
	    *cmdSession,
	    {
	        std::to_string(startTimestamp),
	        std::to_string(threshold),
	    },
	    [callback = std::move(callback)](Session&, Reply reply) {
		    if (const auto* array = std::get_if<reply::Array>(&reply)) {
			    std::vector<ExtendedContact> expiringContacts{};
			    expiringContacts.reserve(array->size());
			    for (const auto contact : *array) {
				    expiringContacts.emplace_back("", std::get<reply::String>(contact).data());
			    }
			    callback(std::move(expiringContacts));
		    }

		    SLOGE << "Fetch expiring contacts script returned unexpected reply: " << StreamableVariant(reply);
	    });
}

} // namespace flexisip