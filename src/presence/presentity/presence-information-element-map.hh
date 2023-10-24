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

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "presence-information-element.hh"
#include "presentity-presence-information-listener.hh"

namespace flexisip {
class ElementMapListener {
public:
	virtual ~ElementMapListener() = default;

	virtual void onMapUpdate() = 0;
};

class PresenceInformationElementMap : public std::enable_shared_from_this<PresenceInformationElementMap> {
public:
	using ElementMapType = std::unordered_map<std::string /*Etag*/, std::unique_ptr<PresenceInformationElement>>;

	static std::shared_ptr<PresenceInformationElementMap>
	make(belle_sip_main_loop_t* belleSipMainloop, const std::weak_ptr<ElementMapListener>& initialListener);

	virtual ~PresenceInformationElementMap() = default;

	void emplace(const std::string& eTag, PresenceInformationElement* element);
	std::optional<PresenceInformationElement*> getByEtag(const std::string& eTag);
	void removeByEtag(const std::string& eTag, bool notifyOther = true);

	void addParentListener(const std::shared_ptr<PresentityPresenceInformationListener>& listener);
	std::shared_ptr<PresentityPresenceInformationListener>
	findPresenceInfoListener(std::shared_ptr<PresentityPresenceInformation>& info);

	/**
	 * WARNING : modify and emptied calling map
	 */
	void mergeInto(std::shared_ptr<PresenceInformationElementMap>& otherMap,
	               const std::weak_ptr<ElementMapListener>& listener);

	const ElementMapType& getElements() const {
		return mInformationElements;
	};

	size_t getSize() const {
		return mInformationElements.size();
	};

	bool isEmpty() const {
		return mInformationElements.empty();
	};

	const std::optional<std::chrono::system_clock::time_point>& getLastActivity() const {
		return mLastActivity;
	};

private:
	explicit PresenceInformationElementMap(belle_sip_main_loop_t* belleSipMainloop,
	                                       const std::weak_ptr<ElementMapListener>& initialListener)
	    : mBelleSipMainloop(belleSipMainloop), mListeners{} {
		mListeners.push_back(initialListener);
	};

	void notifyListeners();

	std::shared_ptr<PresentityPresenceInformationListener> findParentListener(
	    std::function<bool(const std::shared_ptr<PresentityPresenceInformationListener>&)> predicate) const;

	belle_sip_main_loop_t* mBelleSipMainloop;
	ElementMapType mInformationElements;
	std::vector<std::weak_ptr<ElementMapListener>> mListeners;
	std::optional<std::chrono::system_clock::time_point> mLastActivity = std::nullopt;
	BelleSipSourcePtr mLastActivityTimer = nullptr;

	// Used to find cross-subscribe between two users
	mutable std::list<std::weak_ptr<PresentityPresenceInformationListener>> mParentsListeners;
};

} /* namespace flexisip */
