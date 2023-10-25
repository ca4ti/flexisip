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
#include "utils/core-assert.hh"
#include "utils/redis-server.hh"
#include "utils/test-patterns/test.hh"
#include "utils/test-suite.hh"

namespace flexisip::tester {

void test() {
	RedisServer redis{};
	sofiasip::SuRoot root{};
	CoreAssert asserter{root};
	redis::async::Session session{};
	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Disconnected>(session.getState()));
	bool connected = false;
	session.onConnect([&connected](auto status) {
		BC_ASSERT_CPP_EQUAL(status, REDIS_OK);
		connected = true;
	});

	auto* ready =
	    std::get_if<redis::async::Session::Ready>(&session.connect(root.getCPtr(), "localhost", redis.port()));
	BC_HARD_ASSERT(ready != nullptr);
	BC_ASSERT_FALSE(ready->connected());

	BC_ASSERT_TRUE(asserter.iterateUpTo(1, [&connected]() { return connected; }));

	BC_HARD_ASSERT_CPP_EQUAL(std::get_if<redis::async::Session::Ready>(&session.getState()), ready);
	BC_HARD_ASSERT(ready->connected());
	bool returned = false;
	ready->command({"HGETALL", "*"}, [&returned](decltype(session)&, redis::async::Reply reply) {
		const auto* array = std::get_if<redis::reply::Array>(&reply);
		BC_HARD_ASSERT_TRUE(array != nullptr);
		BC_ASSERT_CPP_EQUAL(array->size(), 0);
		returned = true;
	});

	BC_ASSERT_TRUE(asserter.iterateUpTo(1, [&returned]() { return returned; }));
	returned = false;

	ready->command({"HELLO"}, [&returned](decltype(session)&, redis::async::Reply reply) {
		const auto* array = std::get_if<redis::reply::Array>(&reply);
		BC_HARD_ASSERT_TRUE(array != nullptr);
		SLOGD << "Server information: " << *array;
		BC_ASSERT(std::get<redis::reply::String>((*array)[1]) == "redis");
		returned = true;
	});

	BC_ASSERT_TRUE(asserter.iterateUpTo(1, [&returned]() { return returned; }));

	returned = false;
	ready->command({"HGETALL", "*"}, [&returned](decltype(session)&, redis::async::Reply reply) {
		const auto* array = std::get_if<redis::reply::Array>(&reply);
		BC_HARD_ASSERT_TRUE(array != nullptr);
		BC_ASSERT_CPP_EQUAL(array->size(), 0);
		returned = true;
	});
	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Disconnecting>(session.disconnect()));
	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Disconnecting>(session.getState()));
	BC_ASSERT_TRUE(asserter.iterateUpTo(1, [&returned]() { return returned; }));
	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Disconnected>(session.getState()));

	ready = std::get_if<redis::async::Session::Ready>(&session.connect(root.getCPtr(), "localhost", redis.port()));
	BC_HARD_ASSERT(ready != nullptr);

	// Pre-connect command
	returned = false;
	ready->command({"HGETALL", "*"}, [&returned](decltype(session)&, redis::async::Reply reply) {
		BC_ASSERT_TRUE(std::holds_alternative<redis::reply::Array>(reply));
		returned = true;
	});
	BC_ASSERT_TRUE(asserter.iterateUpTo(1, [&returned]() { return returned; }));
	const auto& connectionResult = session.connect(root.getCPtr(), "localhost", redis.port());
	BC_HARD_ASSERT_CPP_EQUAL(std::get_if<redis::async::Session::Ready>(&connectionResult), ready);
	BC_ASSERT(ready->connected()); // Already connected
}

namespace {
TestSuite _("redis::async::Context",
            {
                CLASSY_TEST(test),
            });
}
} // namespace flexisip::tester
