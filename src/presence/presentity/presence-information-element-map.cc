/*
    Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010-2023 Belledonne Communications SARL, All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "presence-information-element-map.hh"

#include "presence/presence-server.hh"

using namespace std;
namespace flexisip {

std::shared_ptr<PresenceInformationElementMap>
PresenceInformationElementMap::make(belle_sip_main_loop_t* belleSipMainloop,
                                    const std::weak_ptr<ElementMapListener>& initialListener) {
	return std::shared_ptr<PresenceInformationElementMap>(
	    new PresenceInformationElementMap(belleSipMainloop, initialListener));
}

void PresenceInformationElementMap::removeByEtag(const std::string& eTag, bool notifyOther) {
	auto it = mInformationElements.find(eTag);
	if (it != mInformationElements.end()) {
		mInformationElements.erase(it);
		mLastActivity = std::chrono::system_clock::now();
		mLastActivityTimer = belle_sip_main_loop_create_cpp_timeout(
		    mBelleSipMainloop,
		    [weakThis = weak_from_this()](unsigned int) {
			    if (auto sharedThis = weakThis.lock()) {
				    sharedThis->mLastActivity = nullopt;
			    }
			    return BELLE_SIP_STOP;
		    },
		    PresenceServer::sLastActivityRetentionMs, "Last activity retention timer");
		if (notifyOther) {
			notifyListeners();
		}
	} else SLOGD << "No tuples found for etag [" << eTag << "]";
}

void PresenceInformationElementMap::emplace(const std::string& eTag, PresenceInformationElement* element) {
	if (mInformationElements.try_emplace(eTag, element).second) {
		notifyListeners();
	}
}

std::optional<PresenceInformationElement*> PresenceInformationElementMap::getByEtag(const std::string& eTag) {
	if (auto it = mInformationElements.find(eTag); it != mInformationElements.end()) {
		return it->second.get();
	}

	return nullopt;
}

void PresenceInformationElementMap::mergeInto(std::shared_ptr<PresenceInformationElementMap>& otherMap,
                                              const std::weak_ptr<ElementMapListener>& listener) {
	otherMap->mInformationElements.merge(mInformationElements);
	otherMap->mListeners.push_back(listener);
	otherMap->notifyListeners();
}

void PresenceInformationElementMap::notifyListeners() {
	for (auto it = mListeners.begin(); it != mListeners.end();) {
		if (auto sharedListener = (*it).lock()) {
			sharedListener->onMapUpdate();
			++it;
		} else {
			it = mListeners.erase(it);
		}
	}
}

} /* namespace flexisip */
