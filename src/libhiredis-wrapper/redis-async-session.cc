/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "redis-async-session.hh"

#include <hiredis/read.h>
#include <ios>
#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

#include "flexisip/logmanager.hh"

#include "libhiredis-wrapper/redis-args-packer.hh"
#include "libhiredis-wrapper/redis-auth.hh"
#include "libhiredis-wrapper/redis-reply.hh"
#include "registrardb-redis-sofia-event.h"
#include "sofia-sip/su_wait.h"
#include "utils/stl-backports.hh"
#include "utils/variant-utils.hh"

using namespace std::string_view_literals;

typedef struct dictEntry {
	void* key;
	void* val;
	struct dictEntry* next;
} dictEntry;

typedef struct dictType {
	unsigned int (*hashFunction)(const void* key);
	void* (*keyDup)(void* privdata, const void* key);
	void* (*valDup)(void* privdata, const void* obj);
	int (*keyCompare)(void* privdata, const void* key1, const void* key2);
	void (*keyDestructor)(void* privdata, void* key);
	void (*valDestructor)(void* privdata, void* obj);
} dictType;
typedef struct dict {
	dictEntry** table;
	dictType* type;
	unsigned long size;
	unsigned long sizemask;
	unsigned long used;
	void* privdata;
} dict;
#define dictHashKey(ht, key) (ht)->type->hashFunction(key)
#define dictCompareHashKeys(ht, key1, key2)                                                                            \
	(((ht)->type->keyCompare) ? (ht)->type->keyCompare((ht)->privdata, key1, key2) : (key1) == (key2))

static dictEntry* dictFind(dict* ht, const void* key) {
	dictEntry* he;
	unsigned int h;

	if (ht->size == 0) return NULL;
	h = dictHashKey(ht, key) & ht->sizemask;
	he = ht->table[h];
	while (he) {
		if (dictCompareHashKeys(ht, key, he->key)) return he;
		he = he->next;
	}
	return NULL;
}

namespace flexisip::redis::async {

Session::Session() {
	std::stringstream prefix{};
	prefix << "redis::async::Context[" << this << "] - ";
	mLogPrefix = prefix.str();
}

Session::State& Session::connect(su_root_t* sofiaRoot, const std::string_view& address, int port) {
	[&]() {
		if (auto* ready = std::get_if<Ready>(&mState)) {
			SLOGD << mLogPrefix << ".connect() called on " << *ready << ". noop.";
			return;
		}

		ContextPtr ctx{redisAsyncConnect(address.data(), port)};
		if (ctx == nullptr) {
			SLOGE << mLogPrefix << "Failed to create context";
			return;
		}

		if (ctx->err) {
			SLOGE << mLogPrefix << "Connection error: " << ctx->err;
			return;
		}

		ctx->data = this;
		int callbackAdded = redisAsyncSetConnectCallback(ctx.get(), [](const redisAsyncContext* ctx, int status) {
			static_cast<Session*>(ctx->data)->onConnect(ctx, status);
		});
		if (callbackAdded == REDIS_ERR) {
			SLOGE << mLogPrefix << "Failed to set connect callback";
			return;
		}

		callbackAdded = redisAsyncSetDisconnectCallback(ctx.get(), [](const redisAsyncContext* ctx, int status) {
			static_cast<Session*>(ctx->data)->onDisconnect(ctx, status);
		});
		if (callbackAdded == REDIS_ERR) {
			SLOGE << mLogPrefix << "Failed to set disconnect callback";
			return;
		}

		if (REDIS_OK != redisSofiaAttach(ctx.get(), sofiaRoot)) {
			SLOGE << mLogPrefix << "Failed to hook into Sofia loop";
			return;
		}

		mState = Ready(std::move(ctx));
	}();
	return mState;
}
Session::State& Session::disconnect() {
	return mState = Match(std::move(mState))
	                    .against([](Ready&& connected) -> State { return Disconnecting(std::move(connected)); },
	                             [](Disconnecting&& disconnecting) -> State { return std::move(disconnecting); },
	                             [](auto&&) -> State { return Disconnected(); });
}

void Session::onConnect(const redisAsyncContext*, int status) {
	mState = Match(std::move(mState))
	             .against(
	                 [&prefix = this->mLogPrefix, status](Ready&& ready) -> State {
		                 if (status == REDIS_OK) {
			                 ready.mConnected = true;
			                 SLOGD << prefix << "Connected";
			                 return std::move(ready);
		                 }

		                 SLOGE << prefix << "Couldn't connect to redis: " << ready.mCtx->errstr;
		                 return Disconnected();
	                 },
	                 [&prefix = this->mLogPrefix, status](auto&& unexpectedState) -> State {
		                 SLOGE << prefix << "onConnect called with status " << status << " while in state "
		                       << unexpectedState;
		                 return std::move(unexpectedState);
	                 });
	if (mOnConnect) mOnConnect(status);
}

void Session::onDisconnect(const redisAsyncContext* ctx, int status) {
	if (status != REDIS_OK) {
		SLOGW << mLogPrefix << "Forcefully disconnecting. Reason: " << ctx->errstr;
	}
	SLOGD << mLogPrefix << "Disconnected. Was in state: " << StreamableVariant(mState);
	mState = Disconnected();
	if (mOnDisconnect) mOnDisconnect(status);
}

Session::Ready::Ready(ContextPtr&& ctx) : mCtx(std::move(ctx)) {
}
Session::Disconnecting::Disconnecting(Ready&& prev) : mCtx(std::move(prev.mCtx)) {
	redisAsyncDisconnect(mCtx.get());
}

int Session::Ready::command(const ArgsPacker& args, CommandCallback&& callback) const {
	return command(args, std::move(callback),
	               [](redisAsyncContext* asyncCtx, void* reply, void* rawCommandData) noexcept {
		               CommandCallback callback(rawCommandData);

		               auto& sessionContext = *static_cast<Session*>(asyncCtx->data);
		               if (callback) {
			               callback(sessionContext, reply::tryFrom(static_cast<const redisReply*>(reply)));
		               }
	               });
}

int SubscriptionSession::Ready::subscribe(const ArgsPacker::Args& args, CommandCallback&& callback) {
	ArgsPacker command{"SUBSCRIBE"};
	command.addArgs(args);
	auto* topic = command.getCArgs()[1];
	auto* channels = mWrapped.mCtx->sub.channels;
	auto* patterns = mWrapped.mCtx->sub.patterns;
	SLOGD << "BEDUG chann size: " << channels->size << " used: " << channels->used << " privdata:" << channels->privdata
	      << " patterns size: " << patterns->size << " used: " << patterns->used << " privdata: " << patterns->privdata;
	auto* existing = dictFind(channels, topic);
	if (existing) {
		SLOGD << "BEDUG Topic: " << topic
		      << " Existing entry: " << static_cast<redisCallback*>(existing->val)->privdata;
	} else {
		SLOGD << "BEDUG Topic: " << topic << " First entry";
	}

	return mWrapped.command(
	    command, std::move(callback), [](redisAsyncContext* asyncCtx, void* rawReply, void* rawCommandData) noexcept {
		    CommandCallback callback(rawCommandData);
		    auto& sessionContext = *static_cast<Session*>(asyncCtx->data);
		    auto reply = reply::tryFrom(static_cast<const redisReply*>(rawReply));
		    SLOGD << "BEDUG about to call " << rawCommandData << " with " << StreamableVariant(reply);
		    bool lastCall = Match(reply).against([](const reply::Disconnected&) { return true; },
		                                         [](const reply::Array& message) {
			                                         try {
				                                         auto type = unwrap<reply::String>(message[0]);
				                                         if (type != "unsubscribe") return false;
				                                         return true;
				                                         auto subscriptionCount = unwrap<reply::Integer>(message[2]);
				                                         return subscriptionCount <= 1;
			                                         } catch (const std::bad_variant_access&) {
				                                         return false;
			                                         }
		                                         },
		                                         [](const auto&) { return false; });
		    if (callback) {
			    callback(sessionContext, std::move(reply));
		    }
		    SLOGD << "BEDUG mv only fn at " << rawCommandData << " last call? " << lastCall;
		    if (!lastCall) callback.leak(); // Let the context live until the next callback
	    });
}
int SubscriptionSession::Ready::unsubscribe(const ArgsPacker::Args& args) {
	ArgsPacker command{"UNSUBSCRIBE"};
	command.addArgs(args);
	return mWrapped.command(command, {}, nullptr);
}

int Session::Ready::auth(std::variant<auth::ACL, auth::Legacy> credentials, CommandCallback&& callback) {
	return command(Match(credentials)
	                   .against(
	                       [](redis::auth::Legacy legacy) -> ArgsPacker {
		                       return {"AUTH", legacy.password};
	                       },
	                       [](redis::auth::ACL acl) -> ArgsPacker {
		                       return {"AUTH", acl.user, acl.password};
	                       }),
	               std::move(callback));
}
int SubscriptionSession::Ready::auth(std::variant<auth::ACL, auth::Legacy> credentials, CommandCallback&& callback) {
	return mWrapped.auth(credentials, std::move(callback));
}

int Session::Ready::command(const ArgsPacker& args, CommandCallback&& callback, redisCallbackFn* fn) const {
	auto* leaked = callback.leak();
	SLOGD << "BEDUG args: " << args << " leaked ctx: " << leaked;
	auto success = redisAsyncCommandArgv(
	    mCtx.get(), fn, leaked, args.getArgCount(),
	    // This const char** signature supposedly suggests that while the array itself is const, its elements are not.
	    // But I don't see a reason the args would be modified by this function, so I assume this is just a mistake.
	    const_cast<const char**>(args.getCArgs()), args.getArgSizes());
	if (success != REDIS_OK) delete leaked;
	return success;
}

Session::State& Session::getState() {
	return mState;
}
const Session::State& Session::getState() const {
	return mState;
}

std::ostream& operator<<(std::ostream& stream, const Session::Disconnected&) {
	return stream << "Disconnected()";
}
std::ostream& operator<<(std::ostream& stream, const Session::Ready& ready) {
	return stream << std::boolalpha << "Ready(connected: " << ready.mConnected << ", ctx: " << ready.mCtx.get() << ")";
}
std::ostream& operator<<(std::ostream& stream, const Session::Disconnecting& disconnecting) {
	return stream << "Disconnecting(ctx: " << disconnecting.mCtx.get() << ")";
}

void Session::ContextDeleter::operator()(redisAsyncContext* ctx) noexcept {
	if (ctx->c.flags & (REDIS_FREEING | REDIS_DISCONNECTING)) {
		// The context is already halfway through freeing/disconnecting and we're probably in a disconnect callback
		return;
	}

	redisAsyncFree(ctx);
}

bool Session::isConnected() {
	return Match(mState).against([](const Ready& ready) { return ready.connected(); },
	                             [](const auto&) { return false; });
}

} // namespace flexisip::redis::async
