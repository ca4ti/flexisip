/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "redis-reply.hh"

#include <cstddef>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "compat/hiredis/async.h"
#include "utils/variant-utils.hh"

namespace flexisip::redis::reply {

Reply tryFrom(const redisReply* reply) {
	switch (reply->type) {
		case REDIS_REPLY_ERROR: {
			return Error{{reply->str, reply->len}};
		} break;
		case REDIS_REPLY_STRING: {
			return String{{reply->str, reply->len}};
		} break;
		case REDIS_REPLY_INTEGER: {
			return reply->integer;
		} break;
		case REDIS_REPLY_ARRAY: {
			return Array{reply->element, reply->elements};
		} break;

		default:
			throw std::runtime_error{"Unimplemented Redis reply type: " + std::to_string(reply->type)};
			break;
	}
}

Reply Array::operator[](std::size_t index) const {
	if (mCount <= index) {
		throw std::out_of_range{"Index out of range on Redis array reply"};
	}
	return tryFrom(mElements[index]);
}

std::ostream& operator<<(std::ostream& stream, const Error& error) {
	return stream << "redis::Error('" << static_cast<const std::string_view&>(error) << "')";
}
std::ostream& operator<<(std::ostream& stream, const String& str) {
	return stream << '"' << static_cast<const std::string_view&>(str) << '"';
}
std::ostream& operator<<(std::ostream& stream, const Array& array) {
	stream << "redis::Array{";
	if (0 < array.size()) {
		stream << "\n";
		for (auto elem : array) {
			stream << "\t" << StreamableVariant(elem) << ",\n";
		}
	}
	return stream << "}";
}

} // namespace flexisip::redis::reply
