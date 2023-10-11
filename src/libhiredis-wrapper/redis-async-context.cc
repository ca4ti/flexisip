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

Context::Context(RedisParameters&& params, std::weak_ptr<void>&& customData)
    : mParams(std::move(params)), mCustomData(std::move(customData)) {
	std::stringstream prefix{};
	prefix << "redis::async::Context[" << this << "] - ";
	mLogPrefix = prefix.str();
}

Context::State& Context::connect(su_root_t* sofiaRoot) {
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
			static_cast<Context*>(ctx->data)->onConnect(ctx, status);
		});
		if (callbackAdded == REDIS_ERR) {
			SLOGE << mLogPrefix << "Failed to set connect callback";
			return;
		}

		callbackAdded = redisAsyncSetDisconnectCallback(ctx.get(), [](const redisAsyncContext* ctx, int status) {
			static_cast<Context*>(ctx->data)->onDisconnect(ctx, status);
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

void Context::onConnect(const redisAsyncContext*, int status) {
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

void Context::onDisconnect(const redisAsyncContext* ctx, int status) {
	if (status != REDIS_OK) {
		SLOGW << mLogPrefix << "Forcefully disconnecting. Reason: " << ctx->errstr;
	}
	SLOGD << mLogPrefix << "Disconnected (was in state: " << StreamableVariant(mState) << ")";
	mState = Disconnected();
}

Context::Connecting::Connecting(ContextPtr&& ctx) : mCtx(std::move(ctx)) {
}
Context::Connected::Connected(Connecting&& prev) : mCtx(std::move(prev.mCtx)) {
}
Context::Disconnecting::Disconnecting(Connected&& prev) {
	redisAsyncDisconnect(prev.mCtx.release());
}

int Context::Connected::sendCommand(redisCallbackFn* fn, const RedisArgsPacker& args, void* privdata) {
	return redisAsyncCommandArgv(
	    mCtx.get(), fn, privdata, args.getArgCount(),
	    // This const char** signature supposedly suggests that while the array itself is const, its elements are not.
	    // But I don't see a reason the args would be modified by this function, so I assume this is just a mistake.
	    const_cast<const char**>(args.getCArgs()), args.getArgSizes());
}

Context::State& Context::getState() {
	return mState;
}
const std::weak_ptr<void>& Context::getCustomData() const {
	return mCustomData;
}

std::ostream& operator<<(std::ostream& stream, const Context::Disconnected&) {
	return stream << "Disconnected()";
}
std::ostream& operator<<(std::ostream& stream, const Context::Connecting& connecting) {
	return stream << "Connecting(ctx: " << connecting.mCtx.get() << ")";
}
std::ostream& operator<<(std::ostream& stream, const Context::Connected& connected) {
	return stream << "Connected(ctx: " << connected.mCtx.get() << ")";
}
std::ostream& operator<<(std::ostream& stream, const Context::Disconnecting&) {
	return stream << "Disconnecting()";
}

void Context::ContextDeleter::operator()(redisAsyncContext* ctx) noexcept {
	if (ctx->c.flags & REDIS_FREEING) {
		// We're in a callback, the context is already being freed
		return;
	}

	redisAsyncFree(ctx);
}

} // namespace flexisip::redis::async
