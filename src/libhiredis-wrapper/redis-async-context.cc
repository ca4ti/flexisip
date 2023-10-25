/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "redis-async-context.hh"

#include <ios>
#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

#include "flexisip/logmanager.hh"

#include "hiredis.h"
#include "libhiredis-wrapper/redis-reply.hh"
#include "registrardb-redis-sofia-event.h"
#include "utils/variant-utils.hh"

using namespace std::string_view_literals;

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

int Session::Ready::command(const ArgsPacker& args, CommandCallback&& callback) {
	return command(args, std::move(callback),
	               [](redisAsyncContext* asyncCtx, void* reply, void* rawCommandData) noexcept {
		               std::unique_ptr<CommandCallback> commandContext{static_cast<CommandCallback*>(rawCommandData)};
		               if (reply == nullptr) return; // Session is being freed

		               auto& sessionContext = *static_cast<Session*>(asyncCtx->data);
		               (*commandContext)(sessionContext, reply::tryFrom(static_cast<const redisReply*>(reply)));
	               });
}

int SubscriptionSession::Ready::subscribe(const ArgsPacker& args, CommandCallback&& callback) {
	return mWrapped.command(args, std::move(callback),
	                        [](redisAsyncContext* asyncCtx, void* rawReply, void* rawCommandData) noexcept {
		                        auto* commandContext{static_cast<CommandCallback*>(rawCommandData)};
		                        if (rawReply == nullptr) { // Session is being freed
			                        delete commandContext;
			                        return;
		                        }
		                        auto* reply = static_cast<const redisReply*>(rawReply);
		                        if (reply->element[0]->str == "unsubscribe"sv) {
			                        delete commandContext;
			                        return;
		                        }

		                        auto& sessionContext = *static_cast<Session*>(asyncCtx->data);
		                        (*commandContext)(sessionContext, reply::tryFrom(reply));
	                        });
}

int Session::Ready::command(const ArgsPacker& args, CommandCallback&& callback, redisCallbackFn* fn) {
	return redisAsyncCommandArgv(
	    mCtx.get(), fn, std::make_unique<CommandCallback>(std::move(callback)).release(), args.getArgCount(),
	    // This const char** signature supposedly suggests that while the array itself is const, its elements are not.
	    // But I don't see a reason the args would be modified by this function, so I assume this is just a mistake.
	    const_cast<const char**>(args.getCArgs()), args.getArgSizes());
}

Session::State& Session::getState() {
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

} // namespace flexisip::redis::async
