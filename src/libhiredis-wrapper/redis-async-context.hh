/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "compat/hiredis/async.h"
#include "hiredis.h"
#include "sofia-sip/su_wait.h"

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

class Session {
public:
	template <typename TContextData>
	friend class SessionWith;

	struct ContextDeleter {
		void operator()(redisAsyncContext*) noexcept;
	};
	using ContextPtr = std::unique_ptr<redisAsyncContext, ContextDeleter>;

	using CommandCallback = std::function<void(Session&, const redisReply*)>;

	class Disconnected {
		friend std::ostream& operator<<(std::ostream&, const Disconnected&);
	};

	class Connecting {
		friend class Session;
		friend std::ostream& operator<<(std::ostream&, const Connecting&);

	private:
		explicit Connecting(ContextPtr&&);
		ContextPtr mCtx;
	};

	class Connected {
	public:
		friend class Session;
		friend std::ostream& operator<<(std::ostream&, const Connected&);

		// SAFETY: Do not use with subscribe
		int command(const RedisArgsPacker& args, CommandCallback&& callback);

	private:
		explicit Connected(Connecting&&);
		ContextPtr mCtx;
	};

	class Disconnecting {
		friend std::ostream& operator<<(std::ostream&, const Disconnecting&);
		friend Session;

	private:
		explicit Disconnecting(Connected&&);
	};

	using State = std::variant<Disconnected, Connecting, Connected, Disconnecting>;

	Session(RedisParameters&&);

	State& getState();
	State& connect(su_root_t*);
	State& disconnect();

private:
	void onConnect(const redisAsyncContext*, int status);
	void onDisconnect(const redisAsyncContext*, int status);

	std::string mLogPrefix{};
	RedisParameters mParams{};
	// Must be the last member of self, to be destructed first. Destructing the ContextPtr calls onDisconnect
	// synchronously, which still needs access to the rest of self.
	State mState{Disconnected()};
};

} // namespace flexisip::redis::async
