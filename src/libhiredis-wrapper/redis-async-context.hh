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
#include "sofia-sip/su_wait.h"

#include "flexisip/sofia-wrapper/waker.hh"

#include "redis-args-packer.hh"
#include "redis-reply.hh"

namespace flexisip::redis::async {

using Reply = reply::Reply;

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

class SubscriptionSession;

class Session {
public:
	template <typename TContextData>
	friend class SessionWith;

	struct ContextDeleter {
		void operator()(redisAsyncContext*) noexcept;
	};
	using ContextPtr = std::unique_ptr<redisAsyncContext, ContextDeleter>;

	using CommandCallback = std::function<void(Session&, Reply)>;

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
		friend class SubscriptionSession;

		// SAFETY: Do not use with subscribe
		int command(const ArgsPacker& args, CommandCallback&& callback);

	private:
		int command(const ArgsPacker&, CommandCallback&&, redisCallbackFn*);
		explicit Connected(Connecting&&);
		ContextPtr mCtx;
	};

	class Disconnecting {
		friend std::ostream& operator<<(std::ostream&, const Disconnecting&);
		friend Session;

	private:
		explicit Disconnecting(Connected&&);
		ContextPtr mCtx;
	};

	using State = std::variant<Disconnected, Connecting, Connected, Disconnecting>;

	Session();

	State& getState();
	State& connect(su_root_t*, const std::string_view& address, int port);
	State& disconnect();

private:
	void onConnect(const redisAsyncContext*, int status);
	void onDisconnect(const redisAsyncContext*, int status);

	std::string mLogPrefix{};
	// Must be the last member of self, to be destructed first. Destructing the ContextPtr calls onDisconnect
	// synchronously, which still needs access to the rest of self.
	State mState{Disconnected()};
};

class SubscriptionSession : Session {
public:
	using CommandCallback = Session::CommandCallback;

	class Connected {
	public:
		int subscribe(const ArgsPacker& args, CommandCallback&& callback);

	private:
		Session::Connected mWrapped;
	};
	static_assert(sizeof(Connected) == sizeof(Session::Connected), "Must be reinterpret_cast-able");

	using State = std::variant<Disconnected, Connecting, Connected, Disconnecting>;

	State& getState() {
		return reinterpret_cast<State&>(Session::getState());
	}
	State& connect(su_root_t* sofiaRoot, const std::string_view& address, int port) {
		return reinterpret_cast<State&>(Session::connect(sofiaRoot, address, port));
	}
	State& disconnect() {
		return reinterpret_cast<State&>(Session::disconnect());
	}
};

} // namespace flexisip::redis::async
