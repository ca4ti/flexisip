/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "eventlogs/call-ended-event-log.hh"

#include "eventlogs/event-log-writer.hh"
#include "eventlogs/identified.hh"
#include "fork-context/branch-info.hh"

namespace flexisip {
using namespace std;

CallEndedEventLog::CallEndedEventLog(const sip_t& sip) : Identified(sip) {
}

void CallEndedEventLog::write(EventLogWriter& writer) const {
	writer.write(*this);
}

} // namespace flexisip
