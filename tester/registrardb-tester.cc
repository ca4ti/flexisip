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

#include "compat/hiredis/hiredis.h"

#include "flexisip/configmanager.hh"
#include "flexisip/module-pushnotification.hh"
#include "flexisip/registrardb.hh"

#include "pushnotification/firebase/firebase-client.hh"

#include "tester.hh"
#include "utils/test-patterns/registrardb-test.hh"

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
	void testExec() noexcept override {
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

/**
 * Should return contacts expiring within [startTimestamp ; startTimestamp + timeRange[
 */
template <typename TDatabase>
class TestFetchExpiringContacts : public RegistrarDbTest<TDatabase> {
	void testExec() noexcept override {
		auto* regDb = RegistrarDb::get();
		auto inserter = ContactInserter(*regDb, *this->mAgent);
		inserter.insert("sip:expected1@te.st", 1s);
		inserter.insert("sip:unexpected@te.st", 3s);
		inserter.insert("sip:expected2@te.st", 2s);
		BC_ASSERT_TRUE(this->waitFor([&inserter] { return inserter.finished(); }, 1s));
		auto targetTimestamp = getCurrentTime() + 1;
		auto timeRange = 2s;

		// Cold loading script
		auto expiringContacts = std::vector<ExtendedContact>();
		regDb->fetchExpiringContacts(targetTimestamp, timeRange, [&expiringContacts](auto&& returnedContacts) {
			expiringContacts = std::move(returnedContacts);
		});

		BC_ASSERT_TRUE(this->waitFor([&expiringContacts] { return !expiringContacts.empty(); }, 1s));
		BC_ASSERT_TRUE(expiringContacts.size() == 2);
		std::unordered_set<std::string> expectedContactStrings = {"sip:expected1@te.st", "sip:expected2@te.st"};
		for (const auto& contact : expiringContacts) {
			// Fail if the returned contact is not in the expected strings
			BC_ASSERT_TRUE(expectedContactStrings.erase(ExtendedContact::urlToString(contact.mSipContact->m_url)) == 1);
		}
		// Assert all expected contacts have been returned
		BC_ASSERT_TRUE(expectedContactStrings.empty());

		// Script should be hot
		expiringContacts.clear();
		regDb->fetchExpiringContacts(targetTimestamp, timeRange, [&expiringContacts](auto&& returnedContacts) {
			expiringContacts = std::move(returnedContacts);
		});

		BC_ASSERT_TRUE(this->waitFor([&expiringContacts] { return !expiringContacts.empty(); }, 1s));
		BC_ASSERT_TRUE(expiringContacts.size() == 2);
		expectedContactStrings = {"sip:expected1@te.st", "sip:expected2@te.st"};
		for (const auto& contact : expiringContacts) {
			// Fail if the returned contact is not in the expected strings
			BC_ASSERT_TRUE(expectedContactStrings.erase(ExtendedContact::urlToString(contact.mSipContact->m_url)) == 1);
		}
		// Assert all expected contacts have been returned
		BC_ASSERT_TRUE(expectedContactStrings.empty());
	}
};

template <typename TDatabase>
class MaxContactsByAorIsHonored : public RegistrarDbTest<TDatabase> {
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

	void testExec() noexcept override {
		auto& uidFields = Record::sLineFieldNames;
		if (uidFields.empty()) uidFields = {"+sip.instance"}; // Do not rely on side-effects from other tests...
		auto previous = Record::sMaxContacts;
		Record::sMaxContacts = 3;
		auto* regDb = RegistrarDb::get();
		ContactInserter inserter(*regDb, *this->mAgent);
		const auto aor = "sip:morethan3@example.org";
		const auto expire = 87s;
		inserter.insert(aor, expire, "sip:existing1@example.org");
		BC_ASSERT_TRUE(this->waitFor([&inserter] { return inserter.finished(); }, 1s));
		inserter.insert(aor, expire, "sip:existing2@example.org");
		BC_ASSERT_TRUE(this->waitFor([&inserter] { return inserter.finished(); }, 1s));
		inserter.insert(aor, expire, "sip:existing3@example.org");
		BC_ASSERT_TRUE(this->waitFor([&inserter] { return inserter.finished(); }, 1s));

		inserter.insert(aor, expire, "sip:onetoomany@example.org");
		BC_ASSERT_TRUE(this->waitFor([&inserter] { return inserter.finished(); }, 1s));

		auto listener = make_shared<TestListener>();
		regDb->fetch(SipUri(aor), listener);
		BC_ASSERT_TRUE(this->waitFor([&record = listener->mRecord]() { return record != nullptr; }, 1s));
		{
			auto& contacts = listener->mRecord->getExtendedContacts();
			BC_ASSERT_EQUAL(contacts.size(), 3, int, "%d");
		}

		Record::sMaxContacts = 5;
		inserter.insert(aor, expire, "sip:added4@example.org");
		BC_ASSERT_TRUE(this->waitFor([&inserter] { return inserter.finished(); }, 1s));
		inserter.insert(aor, expire, "sip:added5@example.org");
		BC_ASSERT_TRUE(this->waitFor([&inserter] { return inserter.finished(); }, 1s));

		listener->mRecord.reset();
		regDb->fetch(SipUri(aor), listener);
		BC_ASSERT_TRUE(this->waitFor([&record = listener->mRecord]() { return record != nullptr; }, 1s));
		{
			auto& contacts = listener->mRecord->getExtendedContacts();
			BC_ASSERT_EQUAL(contacts.size(), 5, int, "%d");
		}

		Record::sMaxContacts = 2;
		inserter.insert(aor, expire, "sip:triggerupdate@example.org");
		BC_ASSERT_TRUE(this->waitFor([&inserter] { return inserter.finished(); }, 1s));

		listener->mRecord.reset();
		regDb->fetch(SipUri(aor), listener);
		BC_ASSERT_TRUE(this->waitFor([&record = listener->mRecord]() { return record != nullptr; }, 1s));
		{
			auto& contacts = listener->mRecord->getExtendedContacts();
			BC_ASSERT_EQUAL(contacts.size(), 2, int, "%d");
		}

		Record::sMaxContacts = previous;
	}
};

class ContactsAreCorrectlyUpdatedWhenMatchedOnUri : public RegistrarDbTest<DbImplementation::Redis> {
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

	void testExec() noexcept override {
		auto previous = Record::sMaxContacts;
		Record::sMaxContacts = 2;
		auto* regDb = RegistrarDb::get();
		sofiasip::Home home{};
		const auto contactBase = ":update-test@example.org";
		const auto contactStr = "sip"s + contactBase;
		const auto contact =
		    sip_contact_create(home.home(), reinterpret_cast<const url_string_t*>(contactStr.c_str()), nullptr);
		const SipUri aor(contactStr);
		BindingParameters params{};
		params.globalExpire = 96;
		params.callId = "insert";
		const auto listener = make_shared<TestListener>();
		regDb->bind(aor, contact, params, listener);
		BC_ASSERT_TRUE(this->waitFor([&record = listener->mRecord]() { return record != nullptr; }, 1s));

		listener->mRecord = nullptr;
		params.callId = "update";
		const auto newContact = sip_contact_create(
		    home.home(), reinterpret_cast<const url_string_t*>((contactStr + ";new=param").c_str()), nullptr);
		regDb->bind(aor, newContact, params, listener);
		BC_ASSERT_TRUE(this->waitFor([&record = listener->mRecord]() { return record != nullptr; }, 1s));

		auto* ctx = redisConnect("127.0.0.1", this->dbImpl.mPort);
		BC_ASSERT_TRUE(ctx && !ctx->err);
		auto* reply = reinterpret_cast<redisReply*>(redisCommand(ctx, "HGETALL fs%s", contactBase));
		BC_ASSERT_PTR_NOT_NULL(reply);
		BC_ASSERT_EQUAL(reply->type, REDIS_REPLY_ARRAY, int, "%i");
		BC_ASSERT_EQUAL(reply->elements, 2, int, "%i");
		BC_ASSERT_EQUAL(reply->element[0]->type, REDIS_REPLY_STRING, int, "%i");
		BC_ASSERT_STRING_EQUAL(reply->element[0]->str, "insert");
		BC_ASSERT_EQUAL(reply->element[1]->type, REDIS_REPLY_STRING, int, "%i");
		std::string serializedContact = reply->element[1]->str;
		BC_ASSERT_TRUE(serializedContact.find("new=param") != std::string::npos);

		// Force inject duplicated contacts inside Redis
		const auto prefix = [&serializedContact] {
			const auto instanceParam = "callid=";
			return serializedContact.substr(0, serializedContact.find(instanceParam) + sizeof(instanceParam) - 1);
		}();
		const auto suffix = serializedContact.substr(prefix.size() + sizeof("insert") - 1);

		std::ostringstream cmd{};
		cmd << "HMSET fs" << contactBase;
		for (const auto& uid : {"duped1", "duped2", "duped3"}) {
			cmd << " " << uid << " " << prefix << uid << suffix;
		}

		auto* insert = reinterpret_cast<redisReply*>(redisCommand(ctx, cmd.str().c_str()));
		freeReplyObject(reply);
		BC_ASSERT_PTR_NOT_NULL(insert);
		BC_ASSERT_EQUAL(insert->type, REDIS_REPLY_STATUS, int, "%i");
		BC_ASSERT_STRING_EQUAL(insert->str, "OK");
		freeReplyObject(insert);

		listener->mRecord.reset();
		regDb->fetch(aor, listener);
		BC_ASSERT_TRUE(this->waitFor([&record = listener->mRecord]() { return record != nullptr; }, 1s));
		{
			auto& contacts = listener->mRecord->getExtendedContacts();
			BC_ASSERT_EQUAL(contacts.size(), 4, int, "%d");
			SLOGD << *listener->mRecord;
		}

		// They are all the same contact, there can be only one
		listener->mRecord.reset();
		params.callId = "trigger-max-aor";
		regDb->bind(aor, contact, params, listener);
		BC_ASSERT_TRUE(this->waitFor([&record = listener->mRecord]() { return record != nullptr; }, 1s));
		{
			auto& contacts = listener->mRecord->getExtendedContacts();
			BC_ASSERT_EQUAL(contacts.size(), 1, int, "%d");
		}

		reply = reinterpret_cast<redisReply*>(redisCommand(ctx, "HGETALL fs%s", contactBase));
		BC_ASSERT_PTR_NOT_NULL(reply);
		BC_ASSERT_EQUAL(reply->type, REDIS_REPLY_ARRAY, int, "%i");
		BC_ASSERT_EQUAL(reply->elements, 2, int, "%i");
		freeReplyObject(reply);

		redisFree(ctx);
		Record::sMaxContacts = previous;
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
	void testExec() noexcept override {
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

static test_t tests[] = {
    TEST_NO_TAG("Fetch expiring contacts on Redis", run<TestFetchExpiringContacts<DbImplementation::Redis>>),
    TEST_NO_TAG("Fetch expiring contacts in Internal DB", run<TestFetchExpiringContacts<DbImplementation::Internal>>),
    TEST_NO_TAG("An AOR cannot contain more than max-contacts-by-aor [Internal]",
                run<MaxContactsByAorIsHonored<DbImplementation::Internal>>),
    TEST_NO_TAG("An AOR cannot contain more than max-contacts-by-aor [Redis]",
                run<MaxContactsByAorIsHonored<DbImplementation::Redis>>),
    TEST_NO_TAG("Contacts are correctly updated when matched on URI [Redis]",
                run<ContactsAreCorrectlyUpdatedWhenMatchedOnUri>),
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
