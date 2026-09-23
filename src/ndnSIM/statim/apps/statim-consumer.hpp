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

#ifndef STATIM_CONSUMER_HPP
#define STATIM_CONSUMER_HPP

#include "ns3/ndnSIM/apps/ndn-consumer.hpp"
#include "ns3/ndnSIM/model/ndn-common.hpp"

#include <map>
#include <set>

namespace ns3
{
namespace ndn
{

class StatimConsumer : public Consumer
{
public:
  static TypeId GetTypeId();

  StatimConsumer();
  virtual ~StatimConsumer();

  virtual void OnData(shared_ptr<const Data> data);

  virtual void OnTimeout(uint32_t sequenceNumber);

  virtual void WillSendOutInterest(uint32_t sequenceNumber);
  uint64_t GetInterestSent() const { return m_interestSent; }
  uint64_t GetUniqueInterestCount() const { return m_seqFirstSent.size(); }
  uint64_t GetDataReceived() const { return m_dataReceived; }
  uint64_t GetTimeoutCount() const { return m_timeoutCount; }

private:
  virtual void StartApplication();

  virtual void StopApplication();

protected:
  virtual void ScheduleNextPacket();

  void SetRandomize(const std::string& value);

  std::string GetRandomize() const;

protected:
  double m_frequency;
  bool m_firstTime;
  Ptr<RandomVariableStream> m_random;
  std::string m_randomType;

  uint64_t m_interestSent;
  uint64_t m_dataReceived;
  uint64_t m_timeoutCount;

  std::map<uint32_t, Time> m_seqSendTime;
  std::map<int64_t, uint32_t> m_timeoutsBySendBin;

  // Track first send times and satisfied sequences. Window loss counts sequences
  // first sent in [LossWindowStart, LossWindowStart+LossWindowLen) that remain
  // unsatisfied at the end of the run.
  std::map<uint32_t, Time> m_seqFirstSent;
  std::set<uint32_t> m_seqSatisfied;
  Time m_lossWindowStart; // 0 = window reporting disabled
  Time m_lossWindowLen;
};

} // namespace ndn
} // namespace ns3

#endif // STATIM_CONSUMER_HPP
