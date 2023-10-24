/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "libhiredis-wrapper/redis-async-context.hh"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <variant>
#include <vector>

#include "bctoolbox/tester.h"

#include "flexisip/sofia-wrapper/su-root.hh"

#include "compat/hiredis/hiredis.h"

#include "libhiredis-wrapper/redis-reply.hh"
#include "registrardb-redis.hh"
#include "utils/core-assert.hh"
#include "utils/redis-server.hh"
#include "utils/test-patterns/test.hh"
#include "utils/test-suite.hh"
#include "utils/variant-utils.hh"

namespace flexisip::tester {

void test() {
	RedisServer redis{};
	sofiasip::SuRoot root{};
	CoreAssert asserter{root};
	redis::async::Session session{{.domain = "localhost", .port = redis.port()}};
	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Disconnected>(session.getState()));

	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Connecting>(session.connect(root.getCPtr())));

	BC_HARD_ASSERT_TRUE(asserter.iterateUpTo(
	    1, [&session]() { return std::holds_alternative<decltype(session)::Connected>(session.getState()); }));

	auto& ready = std::get<decltype(session)::Connected>(session.getState());
	bool returned = false;
	ready.command({"HGETALL", "*"}, [&returned](decltype(session)&, redis::async::Reply reply) {
		const auto* array = std::get_if<redis::reply::Array>(&reply);
		BC_HARD_ASSERT_TRUE(array != nullptr);
		BC_ASSERT_CPP_EQUAL(array->size(), 0);
		returned = true;
	});

	BC_ASSERT_TRUE(asserter.iterateUpTo(1, [&returned]() { return returned; }));
	returned = false;

	ready.command({"HELLO"}, [&returned](decltype(session)&, redis::async::Reply reply) {
		const auto* array = std::get_if<redis::reply::Array>(&reply);
		BC_HARD_ASSERT_TRUE(array != nullptr);
		for (auto elem : *array) {
			Match(elem).against([](const redis::reply::String& str) { SLOGD << "BEDUG STRING " << str; },
			                    [](const redis::reply::Integer& integer) { SLOGD << "BEDUG INTEGER " << integer; },
			                    [](const auto&) { SLOGD << "BEDUG OTHER "; });
		}
		returned = true;
	});

	BC_ASSERT_TRUE(asserter.iterateUpTo(1, [&returned]() { return returned; }));

	returned = false;
	ready.command({"HGETALL", "*"}, [&returned](decltype(session)&, redis::async::Reply reply) {
		const auto* array = std::get_if<redis::reply::Array>(&reply);
		BC_HARD_ASSERT_TRUE(array != nullptr);
		BC_ASSERT_CPP_EQUAL(array->size(), 0);
		returned = true;
	});
	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Disconnecting>(session.disconnect()));
	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Disconnecting>(session.getState()));
	BC_ASSERT_TRUE(asserter.iterateUpTo(1, [&returned]() { return returned; }));
	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Disconnected>(session.getState()));
}

namespace {
TestSuite _("redis::async::Context",
            {
                CLASSY_TEST(test),
            });
}
} // namespace flexisip::tester
