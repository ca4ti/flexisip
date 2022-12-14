/*
    Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010-2022 Belledonne Communications SARL, All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "flexisip/configmanager.hh"
#include "flexisip/module-pushnotification.hh"
#include "flexisip/registrardb.hh"

#include "pushnotification/firebase/firebase-client.hh"

#include "tester.hh"
#include "utils/redis-sync-access.hh"
#include "utils/test-paterns/registrardb-test.hh"

using namespace std;
namespace pn = flexisip::pushnotification;

namespace flexisip {
namespace tester {

// Base class for testing UNSUBSCRIBE/SUBSCRIBE scenario.
// That tests that the subscription is still on if subscribe() methods
// is immediately called after unsubscribe(). We found out that with some
// backend (e.g. Redis) that may lead to a race condition that caused the subscription
// to be off.
template <typename TDatabase>
class SubsequentUnsubscribeSubscribeTest : public RegistrarDbTest<TDatabase> {
protected:
	// Protected types
	struct RegistrarStats : public ContactRegisteredListener {
		void onContactRegistered(const std::shared_ptr<Record>& r, const std::string& uid) override {
			++onContactRegisteredCount;
		}

		int onContactRegisteredCount{0};
	};

	// Protected methods
	void onExec() noexcept {
		auto* regDb = RegistrarDb::get();

		auto stats = make_shared<RegistrarStats>();

		const string topic{"user@sip.example.org"};
		const string uuid{"dummy-uuid"};
		SLOGD << "Subscribing to '" << topic << "'";
		regDb->subscribe(topic, stats);
		this->waitFor(1s);

		SLOGD << "Notifying topic[" << topic << "] with uuid[" << uuid << "]";
		regDb->publish(topic, uuid);
		BC_ASSERT_TRUE(this->waitFor([&stats]() { return stats->onContactRegisteredCount >= 1; }, 1s));
		BC_ASSERT_EQUAL(stats->onContactRegisteredCount, 1, int, "%d");

		SLOGD << "Subsequent Redis UNSUBSCRIBE/SUBSCRIBE";
		regDb->unsubscribe(topic, stats);
		regDb->subscribe(topic, stats);
		this->waitFor(1s);

		SLOGD << "Secondly Notifying topic[" << topic << "] with uuid[" << uuid << "]";
		regDb->publish(topic, uuid);
		BC_ASSERT_TRUE(this->waitFor([&stats]() { return stats->onContactRegisteredCount >= 2; }, 1s));
		BC_ASSERT_EQUAL(stats->onContactRegisteredCount, 2, int, "%d");
	}
};

class RegistrarTester : public RegistrarDbTest<DbImplementation::Redis> {

protected:
	class TestListener : public ContactUpdateListener {
	public:
		virtual void onRecordFound(const std::shared_ptr<Record>& r) override {
			mRecord = r;
		}
		virtual void onError() override {
		}
		virtual void onInvalid() override {
		}
		virtual void onContactUpdated(const std::shared_ptr<ExtendedContact>& ec) override {
		}
		std::shared_ptr<Record> getRecord() const {
			return mRecord;
		}
		void reset() {
			mRecord.reset();
		}

	private:
		std::shared_ptr<Record> mRecord;
	};

	void checkFetch(const std::shared_ptr<Record>& recordAfterBind) {
		auto* regDb = RegistrarDb::get();
		std::shared_ptr<TestListener> listener = make_shared<TestListener>();
		/* Ensure that the Record obtained after fetch operation is the same as the one after the initial bind() */
		regDb->fetch(recordAfterBind->getAor(), listener);
		BC_ASSERT_TRUE(waitFor([listener]() { return listener->getRecord() != nullptr; }, 1s));
		if (listener->getRecord()) {
			BC_ASSERT_TRUE(listener->getRecord()->isSame(*recordAfterBind));
		}
	}
	// Protected methods
	void onExec() noexcept override {
		sofiasip::Home home;
		auto* regDb = RegistrarDb::get();
		std::shared_ptr<TestListener> listener = make_shared<TestListener>();

		SipUri from("sip:bob@example.org");
		BindingParameters params;
		params.globalExpire = 5;
		params.callId = "xyz";

		sip_contact_t* ct;

		/* Add a simple contact */
		ct = sip_contact_create(home.home(), (url_string_t*)"sip:bob@192.168.0.2;transport=tcp", nullptr);
		regDb->bind(from, ct, params, listener);
		BC_ASSERT_TRUE(waitFor([listener]() { return listener->getRecord() != nullptr; }, 1s));
		if (listener->getRecord()) {
			BC_ASSERT_TRUE(listener->getRecord()->getExtendedContacts().size() == 1);
			checkFetch(listener->getRecord());
		}

		/* Remove this contact with an expire parameter */
		listener->reset();
		ct = sip_contact_create(home.home(), (url_string_t*)"sip:bob@192.168.0.2;transport=tcp", "expires=0", nullptr);
		regDb->bind(from, ct, params, listener);
		BC_ASSERT_TRUE(waitFor([listener]() { return listener->getRecord() != nullptr; }, 1s));
		if (listener->getRecord()) {
			BC_ASSERT_TRUE(listener->getRecord()->getExtendedContacts().size() == 0);
			checkFetch(listener->getRecord());
		}

		/* Add a simple contact */
		listener->reset();
		from = SipUri("sip:bobby@example.net");
		ct = sip_contact_create(home.home(), (url_string_t*)"sip:bobby@192.168.0.2;transport=tcp", nullptr);
		regDb->bind(from, ct, params, listener);
		BC_ASSERT_TRUE(waitFor([listener]() { return listener->getRecord() != nullptr; }, 1s));
		if (listener->getRecord()) {
			BC_ASSERT_TRUE(listener->getRecord()->getExtendedContacts().size() == 1);
			checkFetch(listener->getRecord());
		}

		/* Add this contact again (duplicated, without unique id) */
		listener->reset();
		params.callId = "duplicate";
		ct = sip_contact_create(home.home(), (url_string_t*)"sip:bobby@192.168.0.2;transport=tcp;new-param=added",
		                        nullptr);
		regDb->bind(from, ct, params, listener);
		BC_ASSERT_TRUE(waitFor([listener]() { return listener->getRecord() != nullptr; }, 1s));
		if (listener->getRecord()) {
			auto contacts = listener->getRecord()->getExtendedContacts();
			BC_ASSERT_TRUE(contacts.size() == 1);
			BC_ASSERT_STRING_EQUAL(contacts.front()->mSipContact->m_url->url_params, "transport=tcp;new-param=added");
			checkFetch(listener->getRecord());
		}

		/* Remove this contact with an expire parameter but with a different call-id */
		listener->reset();
		ct =
		    sip_contact_create(home.home(), (url_string_t*)"sip:bobby@192.168.0.2;transport=tcp", "expires=0", nullptr);
		params.callId = "abcdef";
		regDb->bind(from, ct, params, listener);
		BC_ASSERT_TRUE(waitFor([listener]() { return listener->getRecord() != nullptr; }, 1s));
		if (listener->getRecord()) {
			BC_ASSERT_TRUE(listener->getRecord()->getExtendedContacts().size() == 0);
			checkFetch(listener->getRecord());
		}

		/* Add a contact with a unique id */
		from = SipUri("sip:alice@example.net");
		listener->reset();
		ct = sip_contact_create(home.home(), (url_string_t*)"sip:alice@10.0.0.2;transport=tcp",
		                        "+sip.instance=\"<urn::uuid::abcd>\"", nullptr);
		regDb->bind(from, ct, params, listener);
		BC_ASSERT_TRUE(waitFor([listener]() { return listener->getRecord() != nullptr; }, 1s));
		if (listener->getRecord()) {
			auto contacts = listener->getRecord()->getExtendedContacts();
			BC_ASSERT_TRUE(contacts.size() == 1);
			BC_ASSERT_TRUE(contacts.front()->getUniqueId() == "\"<urn::uuid::abcd>\"");
			checkFetch(listener->getRecord());
		}

		/* Update this contact */
		listener->reset();
		ct = sip_contact_create(home.home(), (url_string_t*)"sip:alice@10.0.0.3;transport=tcp",
		                        "+sip.instance=\"<urn::uuid::abcd>\"", nullptr);
		regDb->bind(from, ct, params, listener);
		BC_ASSERT_TRUE(waitFor([listener]() { return listener->getRecord() != nullptr; }, 1s));
		if (listener->getRecord()) {
			if (BC_ASSERT_TRUE(listener->getRecord()->getExtendedContacts().size() == 1)) {
				BC_ASSERT_STRING_EQUAL(
				    listener->getRecord()->getExtendedContacts().front()->mSipContact->m_url->url_host, "10.0.0.3");
			}
			checkFetch(listener->getRecord());
		}
	}
};

class ExpiredContactsArePurgedFromRedis : public RegistrarDbTest<DbImplementation::Redis> {
	class TestListener : public ContactUpdateListener {
	public:
		std::shared_ptr<Record> mRecord{nullptr};

		virtual void onRecordFound(const std::shared_ptr<Record>& r) override {
			mRecord = r;
		}
		virtual void onError() override {
		}
		virtual void onInvalid() override {
		}
		virtual void onContactUpdated(const std::shared_ptr<ExtendedContact>& ec) override {
		}
	};

	void onExec() noexcept override {
		auto* regDb = RegistrarDb::get();
		sofiasip::Home home{};
		const auto contactBase = ":expiration-test@example.org";
		const auto contactStr = "sip"s + contactBase;
		const auto contactKey = "expiration-test";
		auto contact =
		    sip_contact_create(home.home(), reinterpret_cast<const url_string_t*>(contactStr.c_str()), nullptr);
		const SipUri aor(contactStr);
		BindingParameters params{};
		params.globalExpire = 239;
		params.callId = contactKey;
		const auto listener = make_shared<TestListener>();
		regDb->bind(aor, contact, params, listener);
		BC_ASSERT_TRUE(this->waitFor([&record = listener->mRecord]() { return record != nullptr; }, 1s));

		RedisSyncContext redis = redisConnect("127.0.0.1", this->dbImpl.mPort);
		auto reply = redis.command("HGETALL fs%s", contactBase);
		BC_ASSERT_EQUAL(reply->type, REDIS_REPLY_ARRAY, int, "%i");
		BC_ASSERT_EQUAL(reply->elements, 2, int, "%i");
		BC_ASSERT_EQUAL(reply->element[0]->type, REDIS_REPLY_STRING, int, "%i");
		BC_ASSERT_STRING_EQUAL(reply->element[0]->str, contactKey);
		BC_ASSERT_EQUAL(reply->element[1]->type, REDIS_REPLY_STRING, int, "%i");
		char* serializedContact = reply->element[1]->str;
		SLOGD << "serializedContact: " << serializedContact;

		// Mangle contact update timestamp inside Redis
		const auto param = "updatedAt=";
		char* index = std::strstr(serializedContact, param) + sizeof(param);
		BC_ASSERT_PTR_NOT_NULL(index);
		*index = '0'; // Rewinding at least 31 years back, that should expire it

		auto insert = redis.command("HMSET fs%s %s %s", contactBase, contactKey, serializedContact);
		BC_ASSERT_EQUAL(insert->type, REDIS_REPLY_STATUS, int, "%i");
		BC_ASSERT_STRING_EQUAL(insert->str, "OK");

		listener->mRecord.reset();
		regDb->fetch(aor, listener);
		BC_ASSERT_TRUE(this->waitFor([&record = listener->mRecord]() { return record != nullptr; }, 1s));
		{
			auto& contacts = listener->mRecord->getExtendedContacts();
			BC_ASSERT_EQUAL(contacts.size(), 1, int, "%d");
		}

		listener->mRecord.reset();
		params.callId = "trigger-cleanup";
		contact = sip_contact_create(home.home(),
		                             reinterpret_cast<const url_string_t*>("sip:trigger-cleanup@example.org"), nullptr);
		regDb->bind(aor, contact, params, listener);
		BC_ASSERT_TRUE(this->waitFor([&record = listener->mRecord]() { return record != nullptr; }, 1s));
		{
			auto& contacts = listener->mRecord->getExtendedContacts();
			BC_ASSERT_EQUAL(contacts.size(), 1, int, "%d");
		}

		{
			auto reply = redis.command("HGETALL fs%s", contactBase);
			BC_ASSERT_EQUAL(reply->type, REDIS_REPLY_ARRAY, int, "%i");
			BC_ASSERT_EQUAL(reply->elements, 2, int, "%i");
			BC_ASSERT_EQUAL(reply->element[0]->type, REDIS_REPLY_STRING, int, "%i");
			BC_ASSERT_STRING_EQUAL(reply->element[0]->str, "trigger-cleanup");
			BC_ASSERT_EQUAL(reply->element[1]->type, REDIS_REPLY_STRING, int, "%i");
			BC_ASSERT_PTR_NOT_NULL(std::strstr(reply->element[1]->str, "sip:trigger-cleanup@example.org"));
		}
	}
};

static test_t tests[] = {
    TEST_NO_TAG("Expired contacts are purged from Redis", run<ExpiredContactsArePurgedFromRedis>),
    TEST_NO_TAG("Subsequent UNSUBSCRIBE/SUBSCRIBE with internal backend",
                run<SubsequentUnsubscribeSubscribeTest<DbImplementation::Internal>>),
    TEST_NO_TAG("Subsequent UNSUBSCRIBE/SUBSCRIBE with Redis backend",
                run<SubsequentUnsubscribeSubscribeTest<DbImplementation::Redis>>),
    TEST_NO_TAG("Registrations with Redis backend", run<RegistrarTester>)};

test_suite_t registarDbSuite = {
    "RegistrarDB",                    // Suite name
    nullptr,                          // Before suite
    nullptr,                          // After suite
    nullptr,                          // Before each test
    nullptr,                          // After each test
    sizeof(tests) / sizeof(tests[0]), // test array length
    tests                             // test array
};

} // namespace tester
} // namespace flexisip
