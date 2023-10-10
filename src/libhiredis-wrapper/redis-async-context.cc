/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "redis-async-context.hh"

#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

#include "flexisip/logmanager.hh"

#include "registrardb-redis.hh"
#include "utils/variant-utils.hh"

namespace flexisip::redis::async {

Context::Context(RedisParameters&& params) : mParams(std::move(params)), mState(Disconnected()) {
	std::stringstream prefix{};
	prefix << "redis::async::Context[" << this << "] - ";
	mLogPrefix = prefix.str();
}

const Context::State& Context::connect() {
	[this]() {
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
		SLOGE << mLogPrefix << "Forcefully disconnecting. Reason: " << ctx->errstr;
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

int Context::Connected::sendCommand(redisCallbackFn* fn, void* privdata, RedisArgsPacker& args) {
	return redisAsyncCommandArgv(mCtx.get(), fn, privdata, args.getArgCount(), args.getCArgs(), args.getArgSizes());
}

} // namespace flexisip::redis::async
