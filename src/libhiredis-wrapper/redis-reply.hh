/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <string_view>
#include <variant>

#include "compat/hiredis/async.h"

namespace flexisip::redis::reply {

class String : public std::string_view {
	friend std::ostream& operator<<(std::ostream&, const String&);
};
class Error : public std::string_view {
public:
	friend std::ostream& operator<<(std::ostream&, const Error&);
};
using Integer = decltype(redisReply::integer);
class Disconnected {
public:
	friend std::ostream& operator<<(std::ostream&, const Disconnected&);
};
class Array;

using Reply = std::variant<String, Array, Integer, Error, Disconnected>;

Reply tryFrom(const redisReply*);

class ArrayOfPairs;

class Array {
public:
	using Element = std::variant<String, Array, Integer>;

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
		Element operator*();

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

	Element operator[](std::size_t) const;

	ArrayOfPairs pairwise() const;

	friend std::ostream& operator<<(std::ostream&, const Array&);

protected:
	const redisReply* const* mElements;
	const std::size_t mCount;
};

class ArrayOfPairs : Array {
public:
	class Iterator {
	public:
		Iterator(const redisReply* const* ptr) : ptr(ptr) {
		}
		Iterator operator++() {
			ptr += 2;
			return *this;
		}
		bool operator!=(const Iterator& other) const {
			return ptr != other.ptr;
		}
		std::pair<Element, Element> operator*();

	private:
		const redisReply* const* ptr;
	};

	explicit ArrayOfPairs(const Array&);

	Iterator begin() const {
		return mElements;
	}
	Iterator end() const {
		return mElements + mCount - 1;
	}

	std::size_t size() const {
		return mCount / 2;
	}

	std::pair<Element, Element> operator[](std::size_t) const;
};

} // namespace flexisip::redis::reply
