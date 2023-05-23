/** Copyright (C) 2010-2023 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "sofia-sip/sip.h"

#include "eventlogs/event-log-write-dispatcher.hh"
#include "eventlogs/identified.hh"
#include "eventlogs/timestamped.hh"

namespace flexisip {

class BranchInfo;

class CallEndedEventLog : public EventLogWriteDispatcher, public Identified, public Timestamped {
public:
	CallEndedEventLog(const sip_t&);

protected:
	void write(EventLogWriter& writer) const override;
};

} // namespace flexisip
