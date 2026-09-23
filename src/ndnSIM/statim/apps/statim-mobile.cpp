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

#include "ns3/ndnSIM/statim/apps/statim-mobile.hpp"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/integer.h"
#include "ns3/log.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"
#include "ns3/node-list.h"

#include "ns3/ndnSIM/helper/ndn-fib-helper.hpp"
#include "ns3/ndnSIM/model/ndn-l3-protocol.hpp"

#include <iomanip>
#include <memory>

NS_LOG_COMPONENT_DEFINE("ndn.statim.StatimMobile");

namespace ns3
{
namespace ndn
{

NS_OBJECT_ENSURE_REGISTERED(StatimMobile);

TypeId StatimMobile::GetTypeId(void)
{
  static TypeId tid =
      TypeId("ns3::ndn::StatimMobile")
          .SetGroupName("Ndn")
          .SetParent<Producer>()
          .AddConstructor<StatimMobile>()

          .AddAttribute("RvPrefix", "Prefix of Rendezvous Point", StringValue("/rv"),
                        MakeNameAccessor(&StatimMobile::m_rvPrefix), MakeNameChecker())
          .AddAttribute("DataPrefix", "Prefix of the data to publish", StringValue("/alice/photo"),
                        MakeNameAccessor(&StatimMobile::m_dataPrefix), MakeNameChecker())
          .AddAttribute("TraceLifetime", "Lifetime for trace Interest packet", StringValue("2s"),
                        MakeTimeAccessor(&StatimMobile::m_traceLifetime), MakeTimeChecker())
          .AddAttribute("RefreshInterval", "Interval between trace interests", StringValue("1s"),
                        MakeTimeAccessor(&StatimMobile::m_refreshInterval), MakeTimeChecker())
          .AddAttribute("TraceDelay", "Delay between OnAssociation and the first trace Interest",
                        StringValue("0.01s"), MakeTimeAccessor(&StatimMobile::m_traceDelay),
                        MakeTimeChecker());
  return tid;
}

StatimMobile::StatimMobile()
    : m_rand(CreateObject<UniformRandomVariable>()), m_traceRetryCnt(0), m_rvInterests(0),
      m_rvData(0), m_interestForData(0), m_data(0), m_waitingForFirstInterest(false),
      m_useExternalAnchor(false), m_traceDelay(Seconds(0.01))
{
  NS_LOG_FUNCTION_NOARGS();
  m_seq = 1;
  m_negotiationDone = true;
}

void StatimMobile::OnAssociation()
{
  NS_LOG_INFO("> STATIM Association done with AP");
  m_waitingForFirstInterest = true;
  Simulator::Schedule(m_traceDelay, &StatimMobile::SendTrace, this);
}

void StatimMobile::StartApplication()
{
  NS_LOG_FUNCTION_NOARGS();
  Producer::StartApplication();
}

void StatimMobile::StopApplication()
{
  if (!m_handoverDelays.empty())
  {
    double sum = 0, maxv = 0, minv = 1e18;
    for (double d : m_handoverDelays)
    {
      sum += d;
      if (d > maxv)
        maxv = d;
      if (d < minv)
        minv = d;
    }
    std::cerr << "\n=== HANDOVER DELAY (TI->firstInterest) ===" << std::endl;
    std::cerr << "Count: " << m_handoverDelays.size() << std::endl;
    std::cerr << "Avg: " << std::fixed << std::setprecision(3) << sum / m_handoverDelays.size()
              << " ms" << std::endl;
    std::cerr << "Min: " << minv << " ms" << std::endl;
    std::cerr << "Max: " << maxv << " ms" << std::endl;
    std::cerr << "All (ms):";
    for (double d : m_handoverDelays)
      std::cerr << " " << std::setprecision(3) << d;
    std::cerr << std::endl;
    std::cerr << "==========================================\n" << std::endl;
  }

  NS_LOG_FUNCTION_NOARGS();
  App::StopApplication();
}

void StatimMobile::SendTrace()
{
  NS_LOG_FUNCTION_NOARGS();

  BOOST_ASSERT(m_seq > 0);

  shared_ptr<Name> name = MakeTracePrefix();
  name->appendSequenceNumber(m_seq);
  m_seq++;

  shared_ptr<Interest> interest = make_shared<Interest>();
  interest->setNonce(m_rand->GetValue(0, std::numeric_limits<uint32_t>::max()));
  interest->setName(*name);
  time::milliseconds interestLifeTime(m_traceLifetime.GetMilliSeconds());
  interest->setInterestLifetime(interestLifeTime);
  m_rvInterests++;
  NS_LOG_INFO("> STATIM Trace Interest sent to " << name->toUri());

  m_transmittedInterests(interest, this, m_face);
  m_appLink->onReceiveInterest(*interest);

  m_lastTraceSendTime = Simulator::Now();

  if (m_traceRefreshEvent.IsRunning())
    Simulator::Cancel(m_traceRefreshEvent);
  if (m_refreshInterval != Seconds(0))
    m_traceRefreshEvent = Simulator::Schedule(Seconds(m_refreshInterval.GetSeconds()),
                                              &StatimMobile::SendTrace, this);

  if (m_traceTimeoutEvent.IsRunning())
    Simulator::Cancel(m_traceTimeoutEvent);
  m_traceTimeoutEvent = Simulator::Schedule(Seconds(m_traceLifetime.GetSeconds()),
                                            &StatimMobile::OnTraceTimeout, this);
}

void StatimMobile::OnData(shared_ptr<const Data> data)
{
  if (!m_active)
    return;

  App::OnData(data);
  NS_LOG_FUNCTION(this << data);

  m_rvData++;

  NS_LOG_INFO("< STATIM DATA for " << data->getName());
  uint64_t seq = data->getName().at(-1).toSequenceNumber();
  if (seq != m_seq - 1)
  {
    NS_LOG_INFO("Wrong seq: " << seq);
    return;
  }

  Simulator::Cancel(m_traceTimeoutEvent);
  m_traceRetryCnt = 0;
}

void StatimMobile::OnTraceTimeout()
{
  NS_LOG_INFO("STATIM Trace interest timed out " << m_seq - 1);
  Simulator::Cancel(m_traceRefreshEvent);

  if (m_traceRetryCnt >= 3)
  {
    m_traceRetryCnt = 0;
    NS_LOG_INFO("STATIM Trace interest timed out 3 times");
    return;
  }

  if (m_seq > 0)
    SendTrace();
  ++m_traceRetryCnt;
}

shared_ptr<Name> StatimMobile::MakeTracePrefix()
{
  shared_ptr<Name> tracePrefix = make_shared<Name>(m_rvPrefix);
  tracePrefix->append("trace");
  tracePrefix->append(m_dataPrefix);
  return tracePrefix; // e.g. /rv/trace/alice/photo
}

void StatimMobile::OnInterest(shared_ptr<const Interest> interest)
{
  App::OnInterest(interest);

  NS_LOG_FUNCTION(this << *interest);

  if (!m_active)
    return;

  if (m_waitingForFirstInterest)
  {
    Time anchor = m_useExternalAnchor ? m_hoAnchor : m_lastTraceSendTime;
    double delayMs = (Simulator::Now() - anchor).GetDouble() / 1e6;
    m_handoverDelays.push_back(delayMs);
    std::cerr << "[HO_DELAY] t=" << std::fixed << std::setprecision(6)
              << Simulator::Now().GetSeconds() << " TI_sent=" << anchor.GetSeconds()
              << " delay=" << std::setprecision(3) << delayMs << "ms" << std::endl;
    m_waitingForFirstInterest = false;
  }

  NS_LOG_INFO("STATIM mobile received Interest for: " << interest->getName());
  m_interestForData++;
  m_data++;

  Name dataName(interest->getName());
  dataName.appendSequenceNumber(m_current);

  auto data = make_shared<Data>();
  data->setName(dataName);
  data->setFreshnessPeriod(::ndn::time::milliseconds(m_freshness.GetMilliSeconds()));
  data->setContent(make_shared<::ndn::Buffer>(m_virtualPayloadSize));

  Signature signature;
  SignatureInfo signatureInfo(static_cast<::ndn::tlv::SignatureTypeValue>(255));
  if (m_keyLocator.size() > 0)
  {
    signatureInfo.setKeyLocator(m_keyLocator);
  }
  signature.setInfo(signatureInfo);
  signature.setValue(::ndn::makeNonNegativeIntegerBlock(::ndn::tlv::SignatureValue, m_signature));
  data->setSignature(signature);

  NS_LOG_INFO("STATIM node(" << GetNode()->GetId()
                             << ") responding with Data: " << data->getName());

  data->wireEncode();

  m_transmittedDatas(data, this, m_face);
  m_appLink->onReceiveData(*data);
}

} // namespace ndn
} // namespace ns3
