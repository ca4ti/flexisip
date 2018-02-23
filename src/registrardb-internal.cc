/*
	Flexisip, a flexible SIP proxy server with media capabilities.
	Copyright (C) 2010-2015  Belledonne Communications SARL, All rights reserved.

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

#include "registrardb.hh"
#include "registrardb-internal.hh"
#include "common.hh"

#include <ctime>
#include <cstdio>
#include <vector>
#include <algorithm>

#include <sofia-sip/sip_protos.h>

using namespace std;

RegistrarDbInternal::RegistrarDbInternal(const string &preferredRoute) : RegistrarDb(preferredRoute) {
}

void RegistrarDbInternal::doBind(const BindParameters &p, const shared_ptr<RegistrarDbListener> &listener) {
	string key = Record::defineKeyFromUrl(p.sip.from);

	if (count_sip_contacts(p.sip.contact) > Record::getMaxContacts()) {
		LOGD("Too many contacts in register %s %i > %i", key.c_str(), count_sip_contacts(p.sip.contact),
			 Record::getMaxContacts());
		listener->onError();
		return;
	}

	time_t now = getCurrentTime();

	map<string, Record *>::iterator it = mRecords.find(key);
	Record *r;
	if (it == mRecords.end()) {
		r = new Record(p.sip.from);
		mRecords.insert(make_pair(key, r));
		LOGD("Creating AOR %s association", key.c_str());
	} else {
		LOGD("AOR %s found", key.c_str());
		r = (*it).second;
	}

	if (r->isInvalidRegister(p.sip.call_id, p.sip.cs_seq)) {
		LOGD("Invalid register");
		listener->onInvalid();
		return;
	}

	const sip_accept_t *accept = p.sip.accept;
	list<string> acceptHeaders;
	while (accept != NULL) {
		acceptHeaders.push_back(accept->ac_type);
		accept = accept->ac_next;
	}

	r->clean(p.sip.contact, p.sip.call_id, p.sip.cs_seq, now, p.version);
	r->update(p.sip.contact, p.sip.path, p.global_expire, p.sip.call_id, p.sip.cs_seq, now, p.alias, acceptHeaders,
			  p.usedAsRoute);

	mLocalRegExpire->update(*r);
	listener->onRecordFound(r);
}

void RegistrarDbInternal::doFetch(const url_t *url, const shared_ptr<RegistrarDbListener> &listener) {
	string key(Record::defineKeyFromUrl(url));
	
	map<string, Record *>::iterator it = mRecords.find(key);
	Record *r = NULL;
	if (it != mRecords.end()) {
		r = (*it).second;
		r->clean(getCurrentTime());
		if (r->isEmpty()) {
			mRecords.erase(it);
			r = NULL;
		}
	}

	listener->onRecordFound(r);
}

void RegistrarDbInternal::doClear(const sip_t *sip, const shared_ptr<RegistrarDbListener> &listener) {
	string key(Record::defineKeyFromUrl(sip->sip_from->a_url));

	if (errorOnTooMuchContactInBind(sip->sip_contact, key, listener)) {
		listener->onError();
		return;
	}

	map<string, Record *>::iterator it = mRecords.find(key);

	if (it == mRecords.end()) {
		listener->onRecordFound(NULL);
		return;
	}

	LOGD("AOR %s found", key.c_str());
	Record *r = (*it).second;

	if (r->isInvalidRegister(sip->sip_call_id->i_id, sip->sip_cseq->cs_seq)) {
		listener->onInvalid();
		return;
	}

	mRecords.erase(it);
	mLocalRegExpire->remove(key);
	delete r;
	listener->onRecordFound(NULL);
}

void RegistrarDbInternal::clearAll() {
	mRecords.clear();
	mLocalRegExpire->clearAll();
}

void RegistrarDbInternal::publish(const std::string &topic, const std::string &uid) {
	LOGD("Publish topic = %s, uid = %s", topic.c_str(), uid.c_str());
	RegistrarDb::notifyContactListener(topic, uid);
}
