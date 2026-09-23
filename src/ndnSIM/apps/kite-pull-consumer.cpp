// Statim simulation modifications, 2026-09-12. Original notices retained below.
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

#include "kite-pull-consumer.hpp"
#include "ns3/ptr.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/callback.h"
#include "ns3/string.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/integer.h"
#include "ns3/double.h"


#include <iomanip>

NS_LOG_COMPONENT_DEFINE("ndn.kite.KitePullConsumer");

namespace ns3 {
namespace ndn {

NS_OBJECT_ENSURE_REGISTERED(KitePullConsumer);

TypeId
KitePullConsumer::GetTypeId(void)
{
  static TypeId tid =
    TypeId("ns3::ndn::KitePullConsumer")
      .SetGroupName("Ndn")
      .SetParent<Consumer>()
      .AddConstructor<KitePullConsumer>()

      .AddAttribute("Frequency", "Frequency of interest packets", StringValue("1.0"),
                    MakeDoubleAccessor(&KitePullConsumer::m_frequency), MakeDoubleChecker<double>())

      .AddAttribute("Randomize",
                    "Type of send time randomization: none (default), uniform, exponential",
                    StringValue("none"), MakeStringAccessor(&KitePullConsumer::SetRandomize,
                                                            &KitePullConsumer::GetRandomize),
                    MakeStringChecker())

      .AddAttribute("MaxSeq", "Maximum sequence number to request",
                    IntegerValue(std::numeric_limits<uint32_t>::max()),
                    MakeIntegerAccessor(&KitePullConsumer::m_seqMax),
                    MakeIntegerChecker<uint32_t>())
      .AddAttribute("LossWindowStart", "Start of the handover loss window",
                    TimeValue(Seconds(0)),
                    MakeTimeAccessor(&KitePullConsumer::m_lossWindowStart), MakeTimeChecker())
      .AddAttribute("LossWindowLen", "Length of the handover loss window",
                    TimeValue(Seconds(4)),
                    MakeTimeAccessor(&KitePullConsumer::m_lossWindowLen), MakeTimeChecker())

    ;

  return tid;
}

KitePullConsumer::KitePullConsumer()
  : m_frequency(1.0)
  , m_firstTime(true)
  , m_interestSent(0)
  , m_dataReceived(0)
  , m_timeoutCount(0)
{
  NS_LOG_FUNCTION_NOARGS();
  m_seqMax = std::numeric_limits<uint32_t>::max();


}

KitePullConsumer::~KitePullConsumer()
{
}

void
KitePullConsumer::StartApplication()
{
  Consumer::StartApplication();
}

void
KitePullConsumer::StopApplication()
{
  std::cerr << "===== KITE Consumer Summary =====" << std::endl;
  std::cerr << "  Sent Interest:    " << m_interestSent << std::endl;
  std::cerr << "  Unique seqs:      " << m_seqFirstSent.size() << std::endl;
  std::cerr << "  Retransmissions:  " << (m_interestSent - m_seqFirstSent.size()) << std::endl;
  std::cerr << "  Received Data:    " << m_dataReceived << std::endl;
  std::cerr << "  Timeout events:   " << m_timeoutCount << std::endl;

  // formula: never_satisfied_unique_seqs / unique_seqs_first_sent_in_window
  if (m_lossWindowStart > Seconds(0)) {
    Time wEnd = m_lossWindowStart + m_lossWindowLen;
    uint32_t sentInWin = 0, neverSat = 0;
    for (const auto& kv : m_seqFirstSent) {
      if (kv.second >= m_lossWindowStart && kv.second < wEnd) {
        ++sentInWin;
        if (m_seqSatisfied.find(kv.first) == m_seqSatisfied.end())
          ++neverSat;
      }
    }
    double frac = sentInWin > 0 ? neverSat * 100.0 / sentInWin : 0.0;
    std::cerr << "[WINDOW_LOSS] formula=never_satisfied_unique_seqs/first_sent_in_window"
              << " window_start=" << m_lossWindowStart.GetSeconds() << "s"
              << " window_len=" << m_lossWindowLen.GetSeconds() << "s"
              << " sent_in_window=" << sentInWin
              << " never_satisfied=" << neverSat
              << " frac_pct=" << std::fixed << std::setprecision(4) << frac
              << std::endl;
  }
  {
    uint32_t neverSatTotal = 0;
    for (const auto& kv : m_seqFirstSent) {
      if (m_seqSatisfied.find(kv.first) == m_seqSatisfied.end())
        ++neverSatTotal;
    }
    std::cerr << "[RUN_LOSS] unique_sent=" << m_seqFirstSent.size()
              << " never_satisfied_total=" << neverSatTotal << std::endl;
  }

  Consumer::StopApplication();
}

void
KitePullConsumer::OnTimeout(uint32_t sequenceNumber)
{
  ++m_timeoutCount;
  Consumer::OnTimeout(sequenceNumber);
}

void
KitePullConsumer::WillSendOutInterest(uint32_t sequenceNumber)
{
  ++m_interestSent;
  if (m_seqFirstSent.find(sequenceNumber) == m_seqFirstSent.end()) {
    m_seqFirstSent[sequenceNumber] = Simulator::Now();
  }
  Consumer::WillSendOutInterest(sequenceNumber);
}

void
KitePullConsumer::ScheduleNextPacket()
{
  // double mean = 8.0 * m_payloadSize / m_desiredRate.GetBitRate ();
  // std::cout << "next: " << Simulator::Now().ToDouble(Time::S) + mean << "s\n";

  if (m_firstTime) {
    m_sendEvent = Simulator::Schedule(Seconds(0.0), &Consumer::SendPacket, this);
    m_firstTime = false;
  }
  else if (!m_sendEvent.IsRunning()) {
    m_sendEvent = Simulator::Schedule((m_random == 0) ? Seconds(1.0 / m_frequency)
                                                      : Seconds(m_random->GetValue()),
                                      &Consumer::SendPacket, this);
  }
}

void
KitePullConsumer::OnData(shared_ptr<const Data> data)
{
  ++m_dataReceived;

  Name dataName = data->getName();

  shared_ptr<Data> newData = make_shared<Data>(data->wireEncode());
  newData->setName(dataName.getPrefix(dataName.size() - 1));

  // Windowed per-sequence loss: mark this sequence satisfied
  if (dataName.size() >= 2) {
    m_seqSatisfied.insert(static_cast<uint32_t>(dataName.at(-2).toSequenceNumber()));
  }

  Consumer::OnData(newData);
}

void
KitePullConsumer::SetRandomize(const std::string& value)
{
  if (value == "uniform") {
    m_random = CreateObject<UniformRandomVariable>();
    m_random->SetAttribute("Min", DoubleValue(0.0));
    m_random->SetAttribute("Max", DoubleValue(2 * 1.0 / m_frequency));
  }
  else if (value == "exponential") {
    m_random = CreateObject<ExponentialRandomVariable>();
    m_random->SetAttribute("Mean", DoubleValue(1.0 / m_frequency));
    m_random->SetAttribute("Bound", DoubleValue(50 * 1.0 / m_frequency));
  }
  else
    m_random = 0;

  m_randomType = value;
}

std::string
KitePullConsumer::GetRandomize() const
{
  return m_randomType;
}

} // namespace ndn
} // namespace ns3
