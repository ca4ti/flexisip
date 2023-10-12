/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>

#include "compat/hiredis/async.h"
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
template <typename TContextData>
class SessionWith;

template <typename TContextData, typename TData>
class CommandContext {
public:
	friend class Command;

	using TMethod = void (TContextData::*)(SessionWith<TContextData>&, const redisReply*, TData&&);

	template <TMethod method>
	void callMethod(TContextData& instance, SessionWith<TContextData>& sessionContext, const redisReply* reply) {
		(instance.*method)(sessionContext, reply, std::move(mData));
	}

private:
	template <typename... Args>
	CommandContext(std::string&& commandStr, Args&&... args)
	    : mCommand(commandStr), mStarted(std::chrono::system_clock::now()), mData(std::forward<Args>(args)...) {
	}

	const std::string mCommand;
	const std::chrono::time_point<std::chrono::system_clock> mStarted;
	TData mData;
};

template <typename TContextData>
class CommandContext<TContextData, void> {
public:
	friend class SessionWith<TContextData>::Command;

	using TMethod = void (TContextData::*)(SessionWith<TContextData>&, const redisReply*);

	template <TMethod method>
	void callMethod(TContextData& instance, SessionWith<TContextData>& sessionContext, const redisReply* reply) {
		(instance.*method)(sessionContext, reply);
	}

private:
	template <typename... Args>
	CommandContext(std::string&& commandStr) : mCommand(commandStr), mStarted(std::chrono::system_clock::now()) {
	}

	const std::string mCommand;
	const std::chrono::time_point<std::chrono::system_clock> mStarted;
};

template <typename TContextData>
class SessionWith {
public:
	template <typename TData>
	class CommandWithData;
	class Command;

	class Connected : Session::Connected {
	public:
		Command command(const RedisArgsPacker& args) {
			return Command(*this, args);
		}

		template <typename TCommandData, typename... DataArgs>
		CommandWithData<TCommandData> command(const RedisArgsPacker& args, DataArgs&&... dataArgs) {
			return CommandWithData<TCommandData>(*this, args, std::forward<DataArgs>(dataArgs)...);
		}
	};
	static_assert(sizeof(Connected) == sizeof(Session::Connected), "Must be reinterpret_cast-able");

	class Command {
	public:
		friend class Connected;

		using Context = CommandContext<TContextData, void>;

		template <typename Context::TMethod method>
		int then() && {
			return std::move(*this).template send<Context, method>(
			    std::unique_ptr<Context>(new Context(mArgs.toString())));
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

				    auto& typedContext = *static_cast<SessionWith<TContextData>*>(asyncCtx->data);
				    if (const auto customData = typedContext.getCustomData().lock()) {
					    try {
						    commandContext->template callMethod<method>(*customData, typedContext,
						                                                static_cast<const redisReply*>(reply));
					    } catch (std::exception& exc) {
						    SLOGE << typedContext.mContext.mLogPrefix
						          << "Unhandled exception in callback: " << exc.what();
					    }
				    }
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
		friend class Connected;

		class CommandContext {
		public:
			friend class CommandWithData;

		private:
			template <typename... Args>
			CommandContext(std::string&& commandStr, Args&&... args)
			    : mCommand(commandStr), mStarted(std::chrono::system_clock::now()), mData(std::forward<Args>(args)...) {
			}

			const std::string mCommand;
			const std::chrono::time_point<std::chrono::system_clock> mStarted;
			TData mData;
		};

		using TMethod = void (TContextData::*)(SessionWith<TContextData>&, const redisReply*, TData&&);

		template <TMethod method>
		int then() && {
			using namespace std::chrono_literals;

			return mCtx.sendCommand(
			    [](redisAsyncContext* asyncCtx, void* reply, void* rawCommandData) noexcept {
				    std::unique_ptr<CommandContext> commandContext{static_cast<CommandContext*>(rawCommandData)};
				    const auto wallClockTime = std::chrono::system_clock::now() - commandContext->mStarted;
				    (wallClockTime < 1s ? SLOGD : SLOGW)
				        << "Redis command completed in "
				        << std::chrono::duration_cast<std::chrono::milliseconds>(wallClockTime).count()
				        << "ms (wall-clock time):\n\t" << commandContext->mCommand;

				    auto& typedContext = *static_cast<SessionWith<TContextData>*>(asyncCtx->data);
				    if (const auto customData = typedContext.getCustomData().lock()) {
					    auto& instance = *customData;
					    try {
						    (instance.*method)(typedContext, static_cast<const redisReply*>(reply),
						                       std::forward<TData>(commandContext->mData));
					    } catch (std::exception& exc) {
						    SLOGE << typedContext.mContext.mLogPrefix
						          << "Unhandled exception in callback: " << exc.what();
					    }
				    }
			    },
			    mArgs, mContext.release());
		}

	private:
		template <typename... DataArgs>
		CommandWithData(Session::Connected& ctx, const RedisArgsPacker& redisArgs, DataArgs&&... dataArgs)
		    : mCtx(ctx), mArgs(redisArgs),
		      mContext(new CommandContext(redisArgs.toString(), std::forward<DataArgs>(dataArgs)...)) {
		}

		Session::Connected& mCtx;
		const RedisArgsPacker& mArgs;
		std::unique_ptr<CommandContext> mContext;
	};

	using State = std::variant<Session::Disconnected, Session::Connecting, Connected, Session::Disconnecting>;

	SessionWith(RedisParameters&& params, std::weak_ptr<TContextData>&& customData)
	    : mContext(std::move(params), std::move(customData)) {
	}

	State& getState() {
		return reinterpret_cast<State&>(mContext.getState());
	}
	const std::weak_ptr<TContextData>& getCustomData() const {
		return reinterpret_cast<const std::weak_ptr<TContextData>&>(mContext.getCustomData());
	}
	State& connect(su_root_t* sofiaRoot) {
		return reinterpret_cast<State&>(mContext.connect(sofiaRoot));
	}

private:
	Session mContext;
};
static_assert(sizeof(SessionWith<std::string>) == sizeof(Session), "Must be reinterpret_cast-able");

} // namespace flexisip::redis::async
