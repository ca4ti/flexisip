/** Copyright (C) 2010-2022 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <memory>
#include <utility>

namespace flexisip {
namespace redis::async {
class Session;
class SubscriptionSession;
} // namespace redis::async
namespace stl_backports {

template <typename T>
class optional {
public:
	constexpr optional() noexcept : mHasValue(false), mDummy() {
	}
	constexpr optional(const optional& other) = default;
	constexpr optional(T value) : mHasValue(true), mValue(value) {
	}

	constexpr explicit operator bool() const noexcept {
		return mHasValue;
	}
	constexpr const T& operator*() const& noexcept {
		return mValue;
	}

private:
	bool mHasValue;

	struct Dummy {};
	union {
		Dummy mDummy;
		T mValue;
	};
};

template <typename Signature>
class move_only_function;

template <typename TReturn, typename... Args>
class move_only_function<TReturn(Args...)> {
public:
	// Give cheat access to implementation details
	friend class redis::async::Session;
	friend class redis::async::SubscriptionSession;

	template <typename TFunction>
	move_only_function(TFunction&& function) : mPtr(std::make_unique<WrappedFunction<TFunction>>(std::move(function))) {
	}
	move_only_function() noexcept = default;

	TReturn operator()(Args&&... args) {
		return (*mPtr)(std::forward<Args>(args)...);
	}

	operator bool() {
		return mPtr;
	}

private:
	class TypeErasedFunction {
	public:
		virtual ~TypeErasedFunction() = default;

		virtual TReturn operator()(Args&&...) = 0;
	};

	template <typename TFunction>
	class WrappedFunction : public TypeErasedFunction {
	public:
		WrappedFunction(TFunction&& function) : mWrapped(std::move(function)) {
		}

		TReturn operator()(Args&&... args) {
			return mWrapped(std::forward<Args>(args)...);
		}

	private:
		TFunction mWrapped;
	};

	explicit move_only_function(void* rawPtr) : mPtr(static_cast<TypeErasedFunction*>(rawPtr)) {
	}
	TypeErasedFunction* leak() {
		return mPtr.release();
	}

	std::unique_ptr<TypeErasedFunction> mPtr{};
};

} // namespace stl_backports
} // namespace flexisip
