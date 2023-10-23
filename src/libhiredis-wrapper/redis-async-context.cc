/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "redis-async-context.hh"

#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

#include "flexisip/logmanager.hh"

#include "registrardb-redis-sofia-event.h"
#include "registrardb-redis.hh"
#include "utils/variant-utils.hh"

namespace flexisip::redis::async {

Session::Session(RedisParameters&& params) : mParams(std::move(params)) {
	std::stringstream prefix{};
	prefix << "redis::async::Context[" << this << "] - ";
	mLogPrefix = prefix.str();
}

Session::State& Session::connect(su_root_t* sofiaRoot) {
	[this, sofiaRoot]() {
		if (std::get_if<Disconnected>(&mState) == nullptr) {
			SLOGE << mLogPrefix << "Cannot connect when in state: " << StreamableVariant(mState);
			return;
		}

		ContextPtr ctx{redisAsyncConnect(mParams.domain.c_str(), mParams.port)};
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

		mState = Connecting(std::move(ctx));
	}();
	return mState;
}
Session::State& Session::disconnect() {
	return mState = Match(std::move(mState))
	                    .against([](Connected&& connected) -> State { return Disconnecting(std::move(connected)); },
	                             [](Disconnecting&& disconnecting) -> State { return std::move(disconnecting); },
	                             [](auto&&) -> State { return Disconnected(); });
}

void Session::onConnect(const redisAsyncContext*, int status) {
	mState = Match(std::move(mState))
	             .against(
	                 [&prefix = this->mLogPrefix, status](Connecting&& connecting) -> State {
		                 if (status == REDIS_OK) {
			                 SLOGD << prefix << "Connected";
			                 return Connected(std::move(connecting));
		                 }

		                 SLOGE << prefix << "Couldn't connect to redis: " << connecting.mCtx->errstr;
		                 return Disconnected();
	                 },
	                 [&prefix = this->mLogPrefix, status](auto&& unexpectedState) -> State {
		                 SLOGE << prefix << "onConnect called with status " << status << " while in state "
		                       << unexpectedState;
		                 return std::move(unexpectedState);
	                 });
}

void Session::onDisconnect(const redisAsyncContext* ctx, int status) {
	if (status != REDIS_OK) {
		SLOGW << mLogPrefix << "Forcefully disconnecting. Reason: " << ctx->errstr;
	}
	SLOGD << mLogPrefix << "Disconnected (was in state: " << StreamableVariant(mState) << ")";
	mState = Disconnected();
}

Session::Connecting::Connecting(ContextPtr&& ctx) : mCtx(std::move(ctx)) {
}
Session::Connected::Connected(Connecting&& prev) : mCtx(std::move(prev.mCtx)) {
}
Session::Disconnecting::Disconnecting(Connected&& prev) {
	redisAsyncDisconnect(prev.mCtx.release());
}

int Session::Connected::command(const RedisArgsPacker& args, CommandCallback&& callback) {
	return redisAsyncCommandArgv(
	    mCtx.get(),
	    [](redisAsyncContext* asyncCtx, void* reply, void* rawCommandData) noexcept {
		    std::unique_ptr<CommandCallback> commandContext{static_cast<CommandCallback*>(rawCommandData)};
		    if (reply == nullptr) return; // Session is being freed

		    auto& typedSession = *static_cast<Session*>(asyncCtx->data);
		    (*commandContext)(typedSession, static_cast<const redisReply*>(reply));
	    },
	    std::make_unique<CommandCallback>(std::move(callback)).release(), args.getArgCount(),
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
std::ostream& operator<<(std::ostream& stream, const Session::Connecting& connecting) {
	return stream << "Connecting(ctx: " << connecting.mCtx.get() << ")";
}
std::ostream& operator<<(std::ostream& stream, const Session::Connected& connected) {
	return stream << "Connected(ctx: " << connected.mCtx.get() << ")";
}
std::ostream& operator<<(std::ostream& stream, const Session::Disconnecting&) {
	return stream << "Disconnecting()";
}

void Session::ContextDeleter::operator()(redisAsyncContext* ctx) noexcept {
	if (ctx->c.flags & REDIS_FREEING) {
		// We're in a callback, the context is already being freed
		return;
	}

	redisAsyncFree(ctx);
}

} // namespace flexisip::redis::async
