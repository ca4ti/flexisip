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
#include "redis-auth.hh"
#include "redis-reply.hh"
#include "utils/stl-backports.hh"

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

	using CommandCallback = stl_backports::move_only_function<void(Session&, Reply)>;

	class Disconnected {
		friend std::ostream& operator<<(std::ostream&, const Disconnected&);
	};

	class Ready {
	public:
		friend class Session;
		friend std::ostream& operator<<(std::ostream&, const Ready&);
		friend class SubscriptionSession;

		int auth(std::variant<auth::ACL, auth::Legacy>, CommandCallback&& callback);
		// SAFETY: Do not use with subscribe
		int command(const ArgsPacker& args, CommandCallback&& callback) const;

		bool connected() const {
			return mConnected;
		}

	private:
		int command(const ArgsPacker&, CommandCallback&&, redisCallbackFn*) const;
		explicit Ready(ContextPtr&&);
		ContextPtr mCtx;
		bool mConnected{false};
	};

	class Disconnecting {
		friend std::ostream& operator<<(std::ostream&, const Disconnecting&);
		friend Session;

	private:
		explicit Disconnecting(Ready&&);
		ContextPtr mCtx;
	};

	using State = std::variant<Disconnected, Ready, Disconnecting>;

	Session();

	State& getState();
	template <typename T>
	T* tryGetState() {
		return std::get_if<T>(&mState);
	}
	const State& getState() const;
	State& connect(su_root_t*, const std::string_view& address, int port);
	State& disconnect();

	bool isConnected();

	void onConnect(std::function<void(int status)>&& handler) {
		mOnConnect = std::move(handler);
	}
	void onDisconnect(std::function<void(int status)>&& handler) {
		mOnDisconnect = std::move(handler);
	}

private:
	void onConnect(const redisAsyncContext*, int status);
	void onDisconnect(const redisAsyncContext*, int status);

	std::string mLogPrefix{};
	std::function<void(int status)> mOnConnect{};
	std::function<void(int status)> mOnDisconnect{};
	// Must be the last member of self, to be destructed first. Destructing the ContextPtr calls onDisconnect
	// synchronously, which still needs access to the rest of self.
	State mState{Disconnected()};
};

// From https://redis.io/commands/subscribe/
// "Once the client enters the subscribed state it is not supposed to issue any other commands, except for
// additional SUBSCRIBE, SSUBSCRIBE, PSUBSCRIBE, UNSUBSCRIBE, SUNSUBSCRIBE, PUNSUBSCRIBE, PING, RESET and QUIT
// commands"
class SubscriptionSession : Session {
public:
	using CommandCallback = Session::CommandCallback;

	class Ready {
	public:
		int auth(std::variant<auth::ACL, auth::Legacy>, CommandCallback&& callback);
		int subscribe(const ArgsPacker::Args& args, CommandCallback&& callback);
		int unsubscribe(const ArgsPacker::Args& args);

	private:
		Session::Ready mWrapped;
	};
	static_assert(sizeof(Ready) == sizeof(Session::Ready), "Must be reinterpret_cast-able");

	using State = std::variant<Disconnected, Ready, Disconnecting>;

	State& getState() {
		return reinterpret_cast<State&>(Session::getState());
	}
	template <typename T>
	T* tryGetState() {
		static_assert(!std::is_same_v<T, Session::Ready>,
		              "Can't let you get a regular session interface from a subscriptions session.");
		if constexpr (std::is_same_v<T, Ready>) {
			return reinterpret_cast<Ready*>(Session::tryGetState<Session::Ready>());
		} else {
			return Session::tryGetState<T>();
		}
	}
	State& connect(su_root_t* sofiaRoot, const std::string_view& address, int port) {
		return reinterpret_cast<State&>(Session::connect(sofiaRoot, address, port));
	}
	State& disconnect() {
		return reinterpret_cast<State&>(Session::disconnect());
	}

	void onConnect(std::function<void(int status)>&& handler) {
		Session::onConnect(std::move(handler));
	}
	void onDisconnect(std::function<void(int status)>&& handler) {
		Session::onDisconnect(std::move(handler));
	}
};
static_assert(sizeof(SubscriptionSession) == sizeof(Session), "Must be reinterpret_cast-able");

} // namespace flexisip::redis::async
