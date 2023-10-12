/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "libhiredis-wrapper/redis-async-context.hh"

#include <cstddef>
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
	bool mReplyReceived = false;

	void onHGETALL(redis::async::SessionWith<TestRedisClient>&, const redisReply* reply) {
		BC_HARD_ASSERT_TRUE(reply != nullptr);
		if (reply->type == REDIS_REPLY_ERROR) {
			BC_HARD_FAIL(reply->str);
		}
		BC_HARD_ASSERT_CPP_EQUAL(reply->type, REDIS_REPLY_ARRAY);
		BC_ASSERT_CPP_EQUAL(reply->len, 0);
		mReplyReceived = true;
	}
};

void test() {
	RedisServer redis{};
	sofiasip::SuRoot root{};
	CoreAssert asserter{root};
	const auto handler = std::make_shared<TestRedisClient>();
	redis::async::SessionWith<TestRedisClient> context({.domain = "localhost", .port = redis.port()}, handler);
	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Disconnected>(context.getState()));

	BC_ASSERT_TRUE(std::holds_alternative<redis::async::Session::Connecting>(context.connect(root.getCPtr())));

	BC_HARD_ASSERT_TRUE(asserter.iterateUpTo(
	    1, [&context]() { return std::holds_alternative<decltype(context)::Connected>(context.getState()); }));

	auto& ready = std::get<decltype(context)::Connected>(context.getState());
	ready.command({"HGETALL", "*"}).then<&TestRedisClient::onHGETALL>();

	BC_ASSERT_TRUE(asserter.iterateUpTo(1, [&replyReceived = handler->mReplyReceived]() { return replyReceived; }));
}

namespace {
TestSuite _("redis::async::Context",
            {
                CLASSY_TEST(test),
            });
}
} // namespace flexisip::tester
