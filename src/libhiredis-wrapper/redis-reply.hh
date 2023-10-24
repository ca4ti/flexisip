/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <string_view>
#include <variant>

#include "compat/hiredis/async.h"

namespace flexisip::redis::reply {

class String : public std::string_view {};
class Error : public std::string_view {};
using Integer = decltype(redisReply::integer);
class Array;

using Reply = std::variant<String, Array, Integer, Error>;

Reply tryFrom(const redisReply*);

class Array {
public:
	class Iterator {
	public:
		Iterator(const redisReply* const* ptr) : ptr(ptr) {
		}
		Iterator operator++() {
			++ptr;
			return *this;
		}
		bool operator!=(const Iterator& other) const {
			return ptr != other.ptr;
		}
		Reply operator*() const {
			return tryFrom(*ptr);
		}

	private:
		const redisReply* const* ptr;
	};

	Array(const redisReply* const* elements, std::size_t count) : mElements(elements), mCount(count) {
	}

	Iterator begin() const {
		return mElements;
	}
	Iterator end() const {
		return mElements + mCount;
	}

	std::size_t size() const {
		return mCount;
	}

	Reply operator[](std::size_t) const;

private:
	const redisReply* const* mElements;
	const std::size_t mCount;
};

} // namespace flexisip::redis::reply
