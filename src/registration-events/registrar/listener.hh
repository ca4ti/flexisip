/*
    Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010-2023  Belledonne Communications SARL, All rights reserved.

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

#include <linphone++/linphone.hh>
#include <memory>

#include "flexisip/registrar/registar-listeners.hh"

namespace flexisip {
namespace RegistrationEvent {
namespace Registrar {

class Listener : public ContactRegisteredListener, public ContactUpdateListener {
public:
	Listener(const std::shared_ptr<linphone::Event>&);
	void onRecordFound(const std::shared_ptr<Record>& r) override;
	void onError() override {
	}
	void onInvalid() override {
	}
	void onContactRegistered(const std::shared_ptr<Record>& r, const std::string& uid) override;
	void onContactUpdated([[maybe_unused]] const std::shared_ptr<ExtendedContact>& ec) override {
	}

private:
	// If the lifetime of the linphone::Core that constructed this event is smaller than that of this Listener, then the
	// belle-sip object is freed regardless of any shared_ptr that this listener might have.
	const std::shared_ptr<linphone::Event> mEvent;
	void processRecord(const std::shared_ptr<Record>& r, const std::string& uidOfFreshlyRegistered);
	// version, previouscontacts
};

} // namespace Registrar
} // namespace RegistrationEvent

} // namespace flexisip
