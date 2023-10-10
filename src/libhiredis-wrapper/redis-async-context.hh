/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <string_view>
#include <variant>

#include "compat/hiredis/async.h"

#include "flexisip/sofia-wrapper/waker.hh"

#include "registrardb-redis.hh"

namespace flexisip::redis::async {

class Reactor {
public:
	virtual void onAddRead() = 0;
	virtual void onDelRead() = 0;
	virtual void onAddWrite() = 0;
	virtual void onDelWrite() = 0;
	virtual void onCleanup() = 0;
};

class SofiaHook : public Reactor {
private:
	void onAddRead() override;
	void onDelRead() override;
	void onAddWrite() override;
	void onDelWrite() override;
	void onCleanup() override;

	sofiasip::Waker mWaker;
};

class Context {
public:
	struct ContextDeleter {
		void operator()(redisAsyncContext* ctx) noexcept {
			// This is safe to call from within a callback. The actual freeing will not take place immediately.
			// So this won't do any harm, even when hiredis plans to free the context at the end of the callback anyway
			// (e.g. on a  failed connection attempt)
			redisAsyncFree(ctx);
		}
	};
	using ContextPtr = std::unique_ptr<redisAsyncContext, ContextDeleter>;

	class Disconnected {
		friend std::ostream& operator<<(std::ostream&, const Disconnected&);
	};

	class Connecting {
		friend class Context;
		friend std::ostream& operator<<(std::ostream&, const Connecting&);

	private:
		explicit Connecting(ContextPtr&&);
		ContextPtr mCtx;
	};

	class Connected {
	public:
		friend class Context;
		friend std::ostream& operator<<(std::ostream&, const Connected&);

	private:
		int sendCommand(redisCallbackFn* fn, void* privdata, RedisArgsPacker& args);

		explicit Connected(Connecting&&);
		ContextPtr mCtx;
	};

	class Disconnecting {
		friend std::ostream& operator<<(std::ostream&, const Disconnecting&);

	private:
		explicit Disconnecting(Connected&&);
	};

	using State = std::variant<Disconnected, Connecting, Connected, Disconnecting>;

	Context(RedisParameters&&);
	~Context();

	const State& getState() const;
	const State& connect();

private:
	void onConnect(const redisAsyncContext*, int status);
	void onDisconnect(const redisAsyncContext*, int status);

	RedisParameters mParams{};
	State mState{Disconnected()};
	std::string mLogPrefix;
};

} // namespace flexisip::redis::async
