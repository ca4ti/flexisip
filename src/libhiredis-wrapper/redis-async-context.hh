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

		int sendCommand(redisCallbackFn* fn, const RedisArgsPacker& args, void* privdata);

	private:
		explicit Connected(Connecting&&);
		ContextPtr mCtx;
	};

	class Disconnecting {
		friend std::ostream& operator<<(std::ostream&, const Disconnecting&);

	private:
		explicit Disconnecting(Connected&&);
	};

	using State = std::variant<Disconnected, Connecting, Connected, Disconnecting>;

	Session(RedisParameters&&, std::weak_ptr<void>&& customData);

	State& getState();
	const std::weak_ptr<void>& getCustomData() const;
	State& connect(su_root_t*);

private:
	void onConnect(const redisAsyncContext*, int status);
	void onDisconnect(const redisAsyncContext*, int status);

	std::string mLogPrefix{};
	RedisParameters mParams{};
	std::weak_ptr<void> mCustomData;
	// Must be the last member of self, to be destructed first. Destructing the ContextPtr calls onDisconnect
	// synchronously, which still needs access to the rest of self.
	State mState{Disconnected()};
};

template <typename TSessionData>
class SessionWith;

template <typename TContextData, typename TData>
class CommandContext;

template <typename TContextData>
class CommandContext<TContextData, void> {
public:
	friend class SessionWith<TContextData>;

	using TMethod = void (TContextData::*)(SessionWith<TContextData>&, const redisReply*);

	template <TMethod method>
	void callMethod(TContextData& instance, SessionWith<TContextData>& sessionContext, const redisReply* reply) {
		(instance.*method)(sessionContext, reply);
	}

protected:
	template <typename... Args>
	CommandContext(std::string&& commandStr) : mCommand(commandStr), mStarted(std::chrono::system_clock::now()) {
	}

	const std::string mCommand;
	const std::chrono::time_point<std::chrono::system_clock> mStarted;
};

template <typename TContextData, typename TData>
class CommandContext : CommandContext<TContextData, void> {
public:
	friend class SessionWith<TContextData>;

	using TMethod = void (TContextData::*)(SessionWith<TContextData>&, const redisReply*, TData&&);

	template <TMethod method>
	void callMethod(TContextData& instance, SessionWith<TContextData>& sessionContext, const redisReply* reply) {
		(instance.*method)(sessionContext, reply, std::move(mData));
	}

private:
	template <typename... Args>
	CommandContext(std::string&& commandStr, Args&&... args)
	    : CommandContext<TContextData, void>(std::move(commandStr)), mData(std::forward<Args>(args)...) {
	}

	TData mData;
};

template <typename TSessionData>
class SessionWith {
public:
	template <typename TData>
	class CommandWithData;
	class Command;

	class Connected : Session::Connected {
	public:
		using TCallback = std::function<void(SessionWith<TSessionData>&, const redisReply*)>;

		void command(const RedisArgsPacker& args, TCallback&& callback) {
			sendCommand(
			    [](redisAsyncContext* asyncCtx, void* reply, void* rawCommandData) noexcept {
				    std::unique_ptr<TCallback> commandContext{static_cast<TCallback*>(rawCommandData)};
				    if (reply == nullptr) return; // Session is being freed

				    auto& typedSession = *static_cast<SessionWith<TSessionData>*>(asyncCtx->data);
				    (*commandContext)(typedSession, static_cast<const redisReply*>(reply));
			    },
			    args, std::make_unique<TCallback>(std::move(callback)).release());
		}
	};
	static_assert(sizeof(Connected) == sizeof(Session::Connected), "Must be reinterpret_cast-able");

	class Command {
	public:
		friend class Connected;
		template <typename TData>
		friend class CommandWithData;

		using Context = CommandContext<TSessionData, void>;

		template <typename Context::TMethod method>
		int then() && {
			return std::move(*this).template with<void>().template then<method>();
		}

		template <typename TData, typename... Args>
		CommandWithData<TData> with(Args&&... args) && {
			return CommandWithData<TData>(std::move(*this), std::forward<Args>(args)...);
		}

	private:
		template <typename TContext, typename TContext::TMethod method>
		int send(std::unique_ptr<TContext>&& commandCtx) && {
			using namespace std::chrono_literals;

			return mCtx.sendCommand(
			    [](redisAsyncContext* asyncCtx, void* reply, void* rawCommandData) noexcept {
				    std::unique_ptr<TContext> commandContext{static_cast<TContext*>(rawCommandData)};
				    const auto wallClockTime = std::chrono::system_clock::now() - commandContext->mStarted;
				    (wallClockTime < 1s ? SLOGD : SLOGW)
				        << "Redis command completed in "
				        << std::chrono::duration_cast<std::chrono::milliseconds>(wallClockTime).count()
				        << "ms (wall-clock time):\n\t" << commandContext->mCommand;

				    auto& typedSession = *static_cast<SessionWith<TSessionData>*>(asyncCtx->data);
				    typedSession.template callMethod<TContext, method>(*commandContext,
				                                                       static_cast<const redisReply*>(reply));
			    },
			    mArgs, commandCtx.release());
		}

		Command(Session::Connected& ctx, const RedisArgsPacker& redisArgs) : mCtx(ctx), mArgs(redisArgs) {
		}

		Session::Connected& mCtx;
		const RedisArgsPacker& mArgs;
	};

	template <typename TData>
	class CommandWithData {
	public:
		friend class Command;

		using Context = CommandContext<TSessionData, TData>;

		template <typename Context::TMethod method>
		int then() && {
			return std::move(mWrapped).template send<Context, method>(std::move(mContext));
		}

	private:
		template <typename... DataArgs>
		CommandWithData(Command&& wrapped, DataArgs&&... dataArgs)
		    : mWrapped(std::move(wrapped)),
		      mContext(new Context(mWrapped.mArgs.toString(), std::forward<DataArgs>(dataArgs)...)) {
		}

		Command mWrapped;
		std::unique_ptr<Context> mContext;
	};

	using State = std::variant<Session::Disconnected, Session::Connecting, Connected, Session::Disconnecting>;

	SessionWith(RedisParameters&& params, std::weak_ptr<TSessionData>&& customData)
	    : mContext(std::move(params), std::move(customData)) {
	}

	State& getState() {
		return reinterpret_cast<State&>(mContext.getState());
	}
	const std::weak_ptr<TSessionData>& getCustomData() const {
		return reinterpret_cast<const std::weak_ptr<TSessionData>&>(mContext.getCustomData());
	}
	State& connect(su_root_t* sofiaRoot) {
		return reinterpret_cast<State&>(mContext.connect(sofiaRoot));
	}

private:
	template <typename TContext, typename TContext::TMethod method>
	void callMethod(TContext& commandContext, const redisReply* reply) {
		if (const auto customData = getCustomData().lock()) {
			try {
				commandContext.template callMethod<method>(*customData, *this, reply);
			} catch (std::exception& exc) {
				SLOGE << mContext.mLogPrefix << "Unhandled exception in callback: " << exc.what();
			}
		}
	}

	Session mContext;
};
static_assert(sizeof(SessionWith<std::string>) == sizeof(Session), "Must be reinterpret_cast-able");

} // namespace flexisip::redis::async
