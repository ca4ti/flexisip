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
#include "libhiredis-wrapper/redis-reply.hh"
#include "recordserializer.hh"
#include "redis-async-script.hh"
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

const auto FETCH_EXPIRING_CONTACTS_SCRIPT = redis::AsyncScript<uint64_t, float>(
#include "fetch-expiring-contacts.lua.hh"
    , // ❯ sed -n '/R"lua(/,/)lua"/p' fetch-expiring-contacts.lua.hh | sed 's/R"lua(//' | head -n-1 | sha1sum
    "8f26674ebf2a65c4eee45d2ae9b98c121cf6ff43");

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

/* This method checks that a redis command was successful, and cleans up if not. You use it with the macro defined
 * below. */

bool RegistrarDbRedisAsync::handleRedisStatus(const string& desc, int redisStatus, RedisRegisterContext* context) {
	if (redisStatus != REDIS_OK) {
		LOGE("Redis error for %s: %d", desc.c_str(), redisStatus);
		if (context != nullptr) {
			context->listener->onError();
			delete context;
		}
		return FALSE;
	}
	return TRUE;
}

#define check_redis_command(cmd, context)                                                                              \
	do {                                                                                                               \
		if (handleRedisStatus(#cmd, (cmd), context) == FALSE) {                                                        \
			return;                                                                                                    \
		}                                                                                                              \
	} while (0)

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

void RegistrarDbRedisAsync::sHandleClear(redisAsyncContext*, redisReply* reply, RedisRegisterContext* context) {
	context->self->handleClear(reply, context);
}

void RegistrarDbRedisAsync::sHandleFetch(redisAsyncContext*, redisReply* reply, RedisRegisterContext* context) {
	context->self->handleFetch(reply, context);
}

void RegistrarDbRedisAsync::sHandleMigration(redisAsyncContext*, redisReply* reply, RedisRegisterContext* context) {
	context->self->handleMigration(reply, context);
}

void RegistrarDbRedisAsync::sHandleRecordMigration(redisAsyncContext*,
                                                   redisReply* reply,
                                                   RedisRegisterContext* context) {
	context->self->handleRecordMigration(reply, context);
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
		if ((cmdSession = std::get_if<Session::Ready>(&state)) == nullptr) return;
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
		    context->succeeded = true;
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

	cmdSession->command({"HGETALL", "fs:" + context->mRecord->getKey()}, [context = std::move(context),
	                                                                      this](Session&, Reply reply) mutable {
		LOGD("Got current Record content for key [fs:%s].", context->mRecord->getKey().c_str());
		reply::Array* array = nullptr;
		if (!(array = std::get_if<reply::Array>(&reply))) {
			SLOGE << "Unexpected reply on Redis pre-bind fetch: " << reply;
			return;
		}

		auto& contacts = context->mRecord->getExtendedContacts();
		auto& changeset = context->mChangeSet;
		// Parse the fetched reply into the Record object (context->mRecord)
		for (auto&& maybeExpired : parseContacts(*array)) {
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
			context->succeeded = true;
			if (context->listener) context->listener->onInvalid();
			return;
		}

		changeset += context->mRecord->applyMaxAor();

		/* now submit the changes triggered by the update operation to REDIS */
		SLOGD << "Sending updated content to REDIS for key [fs:" << context->mRecord->getKey() << "]: " << changeset;
		serializeAndSendToRedis(*context, [this, context = std::move(context)](Session&, Reply reply) mutable {
			handleBind(reply, std::move(context));
		});
	});
}

void RegistrarDbRedisAsync::handleClear(redisReply* reply, std::unique_ptr<RedisRegisterContext>&& context) {
	const char* key = context->mRecord->getKey().c_str();

	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Redis error setting fs:%s [%lu] - %s", key, context->token, reply ? reply->str : "<null reply>");
		if (reply && string(reply->str).find("READONLY") != string::npos) {
			LOGW("Redis couldn't set the AOR because we're connected to a slave. Replying 480.");
			context->succeeded = true;
			if (context->listener) context->listener->onRecordFound(nullptr);
		} else {
			if (context->listener) context->listener->onError();
		}
	} else {
		LOGD("Clearing fs:%s [%lu] success", key, context->token);
		context->succeeded = true;
		if (context->listener) context->listener->onRecordFound(context->mRecord);
	}
	delete context;
}

vector<unique_ptr<ExtendedContact>> RegistrarDbRedisAsync::parseContacts(const reply::Array& reply) {
	decltype(parseContacts(reply)) contacts{};
	const auto entries = reply.pairwise();
	contacts.reserve(entries.size());

	for (const auto [maybeKey, maybeContactStr] : entries) {
		SLOGD << "Parsing contact " << maybeKey << " => " << maybeContactStr;
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
}

void RegistrarDbRedisAsync::doClear(const MsgSip& msg, const shared_ptr<ContactUpdateListener>& listener) {
	auto sip = msg.getSip();
	try {
		// Delete the AOR Hashmap using DEL
		// Once it is done, fetch all the contacts in the AOR and call the onRecordFound of the listener ?
		RedisRegisterContext* context = new RedisRegisterContext(this, SipUri(sip->sip_from->a_url), listener);

		if (!isConnected() && !connect()) {
			LOGE("Not connected to redis server");
			if (context->listener) context->listener->onError();
			delete context;
			return;
		}

		const char* key = context->mRecord->getKey().c_str();
		LOGD("Clearing fs:%s [%lu]", key, context->token);
		mLocalRegExpire->remove(key);
		check_redis_command(mTimedCommand.send(mCommandSession,
		                                       (void (*)(redisAsyncContext*, void*, void*))sHandleClear, context,
		                                       "DEL fs:%s", key),
		                    context);
	} catch (const sofiasip::InvalidUrlError& e) {
		SLOGE << "Invalid 'From' SIP URI [" << e.getUrl() << "]: " << e.getReason();
		context->succeeded = true;
		listener->onInvalid();
	}
}

void RegistrarDbRedisAsync::handleFetch(redisReply* reply, std::unique_ptr<RedisRegisterContext>&& context) {
	const char* key = context->mRecord->getKey().c_str();
	const auto insertIfActive = [&record = context->mRecord](auto&& contact) {
		if (contact->isExpired()) return;

		try {
			record->insertOrUpdateBinding(std::move(contact), nullptr);
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

	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Redis error: %s", reply ? reply->str : "<null reply>");
		if (context->listener) context->listener->onError();
		delete context;
	} else if (reply->type == REDIS_REPLY_ARRAY) {
		// This is the most common scenario: we want all contacts inside the record
		LOGD("GOT fs:%s [%lu] --> %lu contacts", key, context->token, (reply->elements / 2));
		if (reply->elements > 0) {
			for (auto&& maybeExpired : parseContacts(reply)) {
				insertIfActive(maybeExpired);
			}
			context->succeeded = true;
			if (context->listener) context->listener->onRecordFound(context->mRecord);
			delete context;
		} else {
			// We haven't found the record in redis, trying to find an old record
			LOGD("Record fs:%s not found, trying aor:%s", key, key);
			check_redis_command(mTimedCommand.send(mCommandSession,
			                                       (void (*)(redisAsyncContext*, void*, void*))sHandleRecordMigration,
			                                       context, "GET aor:%s", key),
			                    context);
		}
	} else {
		// This is only when we want a contact matching a given gruu
		const char* gruu = context->mUniqueIdToFetch.c_str();
		context->succeeded = true;
		if (reply->len > 0) {
			LOGD("GOT fs:%s [%lu] for gruu %s --> %s", key, context->token, gruu, reply->str);
			insertIfActive(make_unique<ExtendedContact>(gruu, reply->str));
			if (context->listener) context->listener->onRecordFound(context->mRecord);
		} else {
			LOGD("Contact matching gruu %s in record fs:%s not found", gruu, key);
			if (context->listener) context->listener->onRecordFound(nullptr);
		}
		delete context;
	}
}

void RegistrarDbRedisAsync::doFetch(const SipUri& url, const shared_ptr<ContactUpdateListener>& listener) {
	// fetch all the contacts in the AOR (HGETALL) and call the onRecordFound of the listener
	RedisRegisterContext* context = new RedisRegisterContext(this, url, listener);

	if (!isConnected() && !connect()) {
		LOGE("Not connected to redis server");
		if (context->listener) context->listener->onError();
		delete context;
		return;
	}

	const char* key = context->mRecord->getKey().c_str();
	LOGD("Fetching fs:%s [%lu]", key, context->token);
	check_redis_command(mTimedCommand.send(mCommandSession, (void (*)(redisAsyncContext*, void*, void*))sHandleFetch,
	                                       context, "HGETALL fs:%s", key),
	                    context);
}

void RegistrarDbRedisAsync::fetchExpiringContacts(
    time_t startTimestamp, float threshold, std::function<void(std::vector<ExtendedContact>&&)>&& callback) const {
	FETCH_EXPIRING_CONTACTS_SCRIPT.with(startTimestamp, threshold)
	    .then([callback = std::move(callback)](redisReply* reply) {
		    auto count = reply->elements;
		    auto expiringContacts = std::vector<ExtendedContact>();
		    expiringContacts.reserve(count);
		    for (size_t i = 0; i < count; i++) {
			    expiringContacts.emplace_back("", reply->element[i]->str);
		    }
		    callback(std::move(expiringContacts));
	    })
	    .call(mCommandSession);
}

void RegistrarDbRedisAsync::doFetchInstance(const SipUri& url,
                                            const string& uniqueId,
                                            const shared_ptr<ContactUpdateListener>& listener) {
	// fetch only the contact in the AOR (HGET) and call the onRecordFound of the listener
	RedisRegisterContext* context = new RedisRegisterContext(this, url, listener);
	context->mUniqueIdToFetch = uniqueId;

	if (!isConnected() && !connect()) {
		LOGE("Not connected to redis server");
		if (context->listener) context->listener->onError();
		delete context;
		return;
	}

	const char* key = context->mRecord->getKey().c_str();
	const char* field = uniqueId.c_str();
	LOGD("Fetching fs:%s [%lu] contact matching unique id %s", key, context->token, field);
	check_redis_command(mTimedCommand.send(mCommandSession, (void (*)(redisAsyncContext*, void*, void*))sHandleFetch,
	                                       context, "HGET fs:%s %s", key, field),
	                    context);
}

/*
 * The following code is to migrate a redis database to the new way
 */

void RegistrarDbRedisAsync::handleRecordMigration(redisReply* reply, std::unique_ptr<RedisRegisterContext>&& context) {
	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Redis error: %s", reply ? reply->str : "<null reply>");
		context->succeeded = true;
		if (context->listener) context->listener->onRecordFound(nullptr);
	} else {
		if (reply->len > 0) {
			if (!mSerializer->parse(reply->str, reply->len, context->mRecord.get())) {
				LOGE("Couldn't parse stored contacts for aor:%s : %u bytes", context->mRecord->getKey().c_str(),
				     (unsigned int)reply->len);
				context->succeeded = true;
				if (context->listener) context->listener->onRecordFound(nullptr);
			} else {
				LOGD("Parsing stored contacts for aor:%s successful", context->mRecord->getKey().c_str());
				serializeAndSendToRedis(context, sHandleMigration);
				return;
			}
		} else {
			context->succeeded = true;
			// Anchor WKADREGMIGDELREC
			// This is a workaround required in case of unregister (expire set to 0) because
			// if there is only one entry, it will be deleted first so the fetch will come back empty
			// and flexisip will answer 480 instead of 200.
			if (context->listener)
				context->listener->onRecordFound(context->mBindingParameters.globalExpire == 0 ? context->mRecord
				                                                                               : nullptr);
		}
	}
	delete context;
}

void RegistrarDbRedisAsync::handleMigration(redisReply* reply, std::unique_ptr<RedisRegisterContext>&& context) {
	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOGE("Redis error: %s", reply ? reply->str : "<null reply>");
	} else if (reply->type == REDIS_REPLY_ARRAY) {
		LOGD("Fetching all previous records success: %lu record(s) found", (unsigned long)reply->elements);

		for (size_t i = 0; i < reply->elements; i++) {
			redisReply* element = reply->element[i];
			try {
				SipUri url(element->str);
				RedisRegisterContext* new_context = new RedisRegisterContext(this, std::move(url), nullptr);
				LOGD("Fetching previous record: %s", element->str);
				check_redis_command(
				    mTimedCommand.send(mCommandSession,
				                       (void (*)(redisAsyncContext*, void*, void*))sHandleRecordMigration, new_context,
				                       "GET %s", element->str),
				    new_context);
			} catch (const sofiasip::InvalidUrlError& e) {
				LOGD("Skipping invalid previous record [%s]: %s", element->str, e.getReason().c_str());
			}
		}
	} else {
		LOGD("Record aor:%s successfully migrated", context->mRecord->getKey().c_str());
		context->succeeded = true;
		if (context->listener) context->listener->onRecordFound(context->mRecord);
		/*If we want someday to remove the previous record, uncomment the following and comment the delete context above
		check_redis_command(mTimedCommand.send(mContext, (void (*)(redisAsyncContext*, void*, void*))sHandleClear,
		    context, "DEL aor:%s", context->mRecord->getKey().c_str()), context);*/
	}
	delete context;
}

void RegistrarDbRedisAsync::doMigration() {
	if (!isConnected() && !connect()) {
		LOGE("Not connected to redis server");
		return;
	}

	LOGD("Fetching previous record(s)");
	RedisRegisterContext* context = new RedisRegisterContext(this, SipUri(), nullptr);
	check_redis_command(mTimedCommand.send(mCommandSession,
	                                       (void (*)(redisAsyncContext*, void*, void*))sHandleMigration, context,
	                                       "KEYS aor:*"),
	                    context);
}

RedisRegisterContext::~RedisRegisterContext() {
	if (!succeeded && listener) listener->onError();
}

} // namespace flexisip