/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hiredis.h"
#include "libhiredis-wrapper/redis-async-context.hh"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <variant>

#include "bctoolbox/tester.h"

#include "flexisip/sofia-wrapper/su-root.hh"

#include "compat/hiredis/hiredis.h"

#include "read.h"
#include "registrardb-redis.hh"
#include "utils/core-assert.hh"
#include "utils/redis-server.hh"
#include "utils/test-patterns/test.hh"
#include "utils/test-suite.hh"

namespace flexisip::tester {

class TestRedisClient {
public:
	std::uint8_t mReplyReceived = 0;

	void onHGETALLWithData(redis::async::SessionWith<TestRedisClient>&, const redisReply* reply, int&& data) {
		BC_HARD_ASSERT_TRUE(reply != nullptr);
		if (reply->type == REDIS_REPLY_ERROR) {
			BC_HARD_FAIL(reply->str);
		}
		BC_HARD_ASSERT_CPP_EQUAL(reply->type, REDIS_REPLY_ARRAY);
		BC_ASSERT_CPP_EQUAL(reply->len, 0);
		BC_ASSERT_CPP_EQUAL(data, 34);
		mReplyReceived++;
	}

	void onHGETALLWithoutData(redis::async::SessionWith<TestRedisClient>&, const redisReply* reply) {
		BC_HARD_ASSERT_TRUE(reply != nullptr);
		if (reply->type == REDIS_REPLY_ERROR) {
			BC_HARD_FAIL(reply->str);
		}
		BC_HARD_ASSERT_CPP_EQUAL(reply->type, REDIS_REPLY_ARRAY);
		BC_ASSERT_CPP_EQUAL(reply->len, 0);
		mReplyReceived++;
	}
};

void test() {
	RedisServer redis{};
	sofiasip::SuRoot root{};
	CoreAssert asserter{root};
	const auto handler = std::make_shared<TestRedisClient>();
	redis::async::SessionWith<TestRedisClient> session({.domain = "localhost", .port = redis.port()}, handler);
	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Disconnected>(session.getState()));

	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Connecting>(session.connect(root.getCPtr())));

	BC_HARD_ASSERT_TRUE(asserter.iterateUpTo(
	    1, [&session]() { return std::holds_alternative<decltype(session)::Connected>(session.getState()); }));

	auto& ready = std::get<decltype(session)::Connected>(session.getState());
	bool returned = false;
	ready.command({"HGETALL", "*"}, [&returned](decltype(session)&, const redisReply* reply) {
		BC_HARD_ASSERT_TRUE(reply != nullptr);
		if (reply->type == REDIS_REPLY_ERROR) {
			BC_HARD_FAIL(reply->str);
		}
		BC_HARD_ASSERT_CPP_EQUAL(reply->type, REDIS_REPLY_ARRAY);
		BC_ASSERT_CPP_EQUAL(reply->len, 0);
		returned = true;
	});

	BC_ASSERT_TRUE(asserter.iterateUpTo(1, [&returned]() { return returned; }));

	returned = false;
	ready.command({"HGETALL", "*"}, [&returned](decltype(session)&, const redisReply* reply) {
		BC_HARD_ASSERT_TRUE(reply != nullptr);
		if (reply->type == REDIS_REPLY_ERROR) {
			BC_HARD_FAIL(reply->str);
		}
		BC_HARD_ASSERT_CPP_EQUAL(reply->type, REDIS_REPLY_ARRAY);
		BC_ASSERT_CPP_EQUAL(reply->len, 0);
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
