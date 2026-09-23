/**
 * Copyright (c) 2011-2015  Regents of the University of California.
 *
 * This file is part of ndnSIM. See AUTHORS for complete list of ndnSIM authors and
 * contributors.
 *
 * ndnSIM is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * ndnSIM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ndnSIM, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 **/

// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for the Statim simulation artifact, 2026-09-12.
// Upstream-derived portions: see THIRD-PARTY.md (repository root).
// Statim modifications and additional linking permission: see LICENSE.md.

#ifndef STATIM_MOBILE_HPP
#define STATIM_MOBILE_HPP

#include "ns3/ndnSIM/apps/ndn-producer.hpp"
#include "ns3/ndnSIM/model/ndn-common.hpp"
#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/random-variable-stream.h"

#include <vector>

namespace ns3
{
namespace ndn
{

class StatimMobile : public Producer
{
public:
  static TypeId GetTypeId(void);

  StatimMobile();

  void OnAssociation();

  void SendTrace();

  virtual void OnInterest(shared_ptr<const Interest> interest);

  virtual void OnData(shared_ptr<const Data> data);

  void OnTraceTimeout();

  shared_ptr<Name> MakeTracePrefix();

protected:
  virtual void StartApplication();

  virtual void StopApplication();

private:
  Name m_rvPrefix;
  Name m_dataPrefix;

  Time m_traceLifetime;
  Time m_refreshInterval;

  Ptr<UniformRandomVariable> m_rand;

  uint64_t m_seq;

  EventId m_traceRefreshEvent;
  EventId m_traceTimeoutEvent;

  int m_traceRetryCnt;
  bool m_negotiationDone;

public:
  int m_rvInterests;
  int m_rvData;
  int m_interestForData;
  int m_data;
  int m_current; // current AP node ID

  // Handover delay: configured handover anchor -> first subsequent Interest.
  // Without an external anchor, measure from the Trace Interest send time.
  // Measurement-only mirror of the KitePullMobile instrumentation so both
  // protocols report [HO_DELAY] with the identical definition.
  Time m_lastTraceSendTime;
  bool m_waitingForFirstInterest;
  std::vector<double> m_handoverDelays; // ms

  // Use the scenario's scheduled handover instant as the latency anchor.
  void MarkHandover(Time anchor)
  {
    m_waitingForFirstInterest = true;
    m_useExternalAnchor = true;
    m_hoAnchor = anchor;
  }
  bool m_useExternalAnchor;
  Time m_hoAnchor;

private:
  Time m_traceDelay; // OnAssociation -> first TI delay (attribute TraceDelay)
};

} // namespace ndn
} // namespace ns3

#endif // STATIM_MOBILE_HPP
