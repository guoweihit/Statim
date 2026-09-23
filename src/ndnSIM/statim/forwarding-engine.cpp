// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/ndnSIM/statim/forwarding-engine.hpp"

#include "ns3/ndnSIM/model/ndn-block-header.hpp"
#include "ns3/ndnSIM/model/ndn-l3-protocol.hpp"
#include <ndn-cxx/lp/tags.hpp>

#include <iomanip>
#include <stdexcept>

NS_LOG_COMPONENT_DEFINE("statim.Engine");

namespace ns3
{
namespace ndn
{
namespace statim
{

namespace
{

inline bool isTraceDataPacket(const ::ndn::Data& data)
{
  return statim::judgeNameIsTrace(data.getName());
}

inline uint64_t getTraceSequence(const ::ndn::Data& data)
{
  return data.getName().at(-1).toSequenceNumber();
}

inline ::ndn::Name getTraceDataPrefix(const ::ndn::Data& data)
{
  auto p = statim::extractDataNameFromTrace(data.getName());
  return p != nullptr ? *p : ::ndn::Name();
}

} // namespace

ForwardingEngine::ForwardingEngine()
    : m_statefulDelay(NanoSeconds(0)), m_configuredFlowTableLifetime(Seconds(0)),
      m_configuredTemporaryLifetime(Seconds(0)), m_enableTemporaryFib(true),
      m_enableInterestReforwarding(true), m_interestReforwardingLimit(30), m_nodeId(0), m_experimentTelemetryEnabled(false),
      m_tempSteerPending(false), m_flowSteerPending(false), m_tempSteerSeq(0), m_flowSteerSeq(0)
{
  m_statefulModule.getTemporaryController().setFlowTableSyncRequestCallback(
      [this](const ControlMessage& msg) { m_centralController.submitFlowUpdate(msg); });

  m_centralController.setControllerDelay(
      m_statefulModule.getTemporaryController().getControllerDelay());
  m_centralController.setFlowModApplyCallback([this](const ControlMessage& msg)
                                              { return this->onFlowTableFlowMod(msg); });

  m_centralController.setSyncAckCallback(
      [this](const ControlMessage& msg)
      { m_statefulModule.getTemporaryController().handleControlResponse(msg); });
}

void ForwardingEngine::setControllerDelay(Time delay)
{
  m_centralController.setControllerDelay(delay);
}

void ForwardingEngine::validateRouteLifetime(Time lifetime) const
{
  if ((m_configuredFlowTableLifetime > Seconds(0) && lifetime != m_configuredFlowTableLifetime) ||
      (m_configuredTemporaryLifetime > Seconds(0) && lifetime != m_configuredTemporaryLifetime))
    throw std::invalid_argument("Both configured FIB lifetimes must equal the saved TI lifetime");
}

void ForwardingEngine::setFlowTableFibEntryLifetime(Time lifetime)
{
  if (lifetime < Seconds(0))
    throw std::invalid_argument("Route lifetime must be nonnegative");
  m_configuredFlowTableLifetime = lifetime;
}

void ForwardingEngine::setTemporaryFibEntryLifetime(Time lifetime)
{
  if (lifetime < Seconds(0))
    throw std::invalid_argument("Route lifetime must be nonnegative");
  m_configuredTemporaryLifetime = lifetime;
  m_statefulModule.getTemporaryController().setEntryLifetime(lifetime);
}

void ForwardingEngine::onReceiveRawPacket(uint16_t inPort, Ptr<const Packet> p)
{
  Ptr<Packet> packet = p->Copy();

  PacketHeader statimHeader;
  if (packet->PeekHeader(statimHeader) != PacketHeader::SERIALIZED_SIZE)
  {
    ++m_counters.malformedPackets;
    return;
  }
  statimHeader.setPacketInPort(inPort);

  PipelineResult pass1 = m_statefulModule.getSwitch().processPacket(statimHeader);

  if (pass1.packetIn)
  {
    NS_LOG_DEBUG("ForwardingEngine: Control packet -> controller");
    return;
  }
  if (pass1.outputPorts.count(StatimPort::STATEFUL_MODULE) > 0)
  {
    NS_LOG_DEBUG("ForwardingEngine: pass1 -> stateful module, port=" << inPort);

    packet->RemoveHeader(statimHeader);
    try
    {
      BlockHeader blockHeader;
      packet->RemoveHeader(blockHeader);
      const ::ndn::Block block = blockHeader.getBlock();
      if (block.type() == ::ndn::tlv::Interest)
      {
        auto interest = std::make_shared<::ndn::Interest>(block);
        processStatefulInterest(inPort, interest, statimHeader);
      }
      else if (block.type() == ::ndn::tlv::Data)
      {
        auto data = std::make_shared<::ndn::Data>(block);
        processStatefulData(inPort, data, statimHeader);
      }
      else
      {
        ++m_counters.malformedPackets;
      }
    }
    catch (const ::ndn::tlv::Error& error)
    {
      ++m_counters.malformedPackets;
      NS_LOG_DEBUG("Discard malformed NDN payload: " << error.what());
    }
  }
  else
  {
    NS_LOG_DEBUG("ForwardingEngine: pass1 -> no route / drop");
    ++m_counters.noRouteRejections;
  }
}

void ForwardingEngine::sendToPort(uint16_t outPort, Ptr<Packet> packet)
{
  Ptr<NetDevice> dev = m_portTable.getDevice(outPort);
  if (dev == nullptr)
  {
    NS_LOG_WARN("ForwardingEngine: no device for port " << outPort);
    return;
  }
  dev->Send(packet, dev->GetBroadcast(), L3Protocol::ETHERNET_FRAME_TYPE);
}

void ForwardingEngine::sendInterestToPort(uint16_t outPort, const ::ndn::Interest& interest)
{
  BlockHeader blockHeader(interest.wireEncode());
  Ptr<Packet> ns3Pkt = Create<Packet>();
  ns3Pkt->AddHeader(blockHeader);
  PacketHeader outHeader(interest.getName());
  outHeader.setType(PacketType::UNPROCESSED);
  outHeader.setOutPorts(0);
  ns3Pkt->AddHeader(outHeader);
  sendToPort(outPort, ns3Pkt);
}

void ForwardingEngine::sendDataToPort(uint16_t outPort, const ::ndn::Data& data)
{
  BlockHeader blockHeader(data.wireEncode());
  Ptr<Packet> ns3Pkt = Create<Packet>();
  ns3Pkt->AddHeader(blockHeader);
  PacketHeader outHeader(data.getName());
  outHeader.setType(PacketType::UNPROCESSED);
  outHeader.setOutPorts(0);
  auto hopCountTag = data.getTag<::ndn::lp::HopCountTag>();
  if (hopCountTag != nullptr)
  {
    outHeader.setSwitchId(static_cast<uint8_t>(*hopCountTag));
  }
  ns3Pkt->AddHeader(outHeader);
  sendToPort(outPort, ns3Pkt);
}

void ForwardingEngine::processStatefulInterest(uint16_t inPort,
                                               std::shared_ptr<::ndn::Interest> interest,
                                               PacketHeader header)
{
  NS_LOG_DEBUG("ForwardingEngine stateful Interest port=" << inPort
                                                          << " name=" << interest->getName());
  ++m_counters.interestsReceived;

  ::ndn::Name name = interest->getName();

  if (m_experimentTelemetryEnabled && statim::judgeNameIsTrace(name))
  {
    ++m_counters.traceInterestsHandled;
    std::cerr << "[TRACE_TI] t=" << std::fixed << std::setprecision(9)
              << Simulator::Now().GetSeconds() << " nodeId=" << m_nodeId << " name=" << name
              << std::endl;
  }

  const auto pitResult = m_pit.insert(*interest, inPort);
  if (!pitResult.accepted())
  {
    NS_LOG_DEBUG("  duplicate nonce, drop");
    ++m_counters.pitDuplicates;
    return;
  }

  auto cached = m_cs.find(name);
  if (cached != nullptr)
  {
    NS_LOG_DEBUG("  CS hit");
    ++m_counters.csHits;
    sendDataToPort(inPort, *cached);
    return;
  }

  header.computeNameHashes(name);
  header.setPacketInPort(inPort);

  Simulator::Schedule(m_statefulDelay, &ForwardingEngine::processInterestAfterDelay, this, inPort,
                      interest, header);
}

void ForwardingEngine::processInterestAfterDelay(uint16_t inPort,
                                                 std::shared_ptr<::ndn::Interest> interest,
                                                 PacketHeader header)
{
  auto* entry = m_statefulModule.getTemporaryFib().findLongestPrefix(interest->getName());

  bool usedTemporaryFib = false;
  if (entry != nullptr && m_enableTemporaryFib)
  {
    std::set<uint16_t> ports;
    for (const auto& hop : entry->getNextHops())
    {
      ports.insert(hop.second.port);
    }

    if (ports.empty())
    {
      ++m_counters.statimLookupMisses;
      return;
    }

    header.setType(PacketType::INTEREST);
    header.setOutPorts(StatefulForwarder::encodeOutPortsMask(ports));
    usedTemporaryFib = true;
  }
  else
  {
    header.setType(PacketType::INTEREST);
    header.setOutPorts(0);
  }

  PipelineResult pass2 = m_statefulModule.getSwitch().processPacket(header);
  if (pass2.dropped || pass2.outputPorts.empty())
  {
    ++m_counters.statimLookupMisses;
    ++m_counters.noRouteRejections;
    return;
  }

  for (uint16_t port : pass2.outputPorts)
  {
    if (port == inPort)
      continue;

    if (m_experimentTelemetryEnabled && usedTemporaryFib && m_tempSteerPending &&
        m_tempSteerPrefix.isPrefixOf(interest->getName()))
    {
      std::cerr << "[STEER_FIRST] t=" << std::fixed << std::setprecision(9)
                << Simulator::Now().GetSeconds() << " nodeId=" << m_nodeId << " source=temporaryFib"
                << " seq=" << m_tempSteerSeq << " prefix=" << m_tempSteerPrefix << std::endl;
      m_tempSteerPending = false;
    }
    else if (m_experimentTelemetryEnabled && !usedTemporaryFib && m_flowSteerPending &&
             m_flowSteerPrefix.isPrefixOf(interest->getName()))
    {
      std::cerr << "[STEER_FIRST] t=" << std::fixed << std::setprecision(9)
                << Simulator::Now().GetSeconds() << " nodeId=" << m_nodeId << " source=flowTableFib"
                << " seq=" << m_flowSteerSeq << " prefix=" << m_flowSteerPrefix << std::endl;
      m_flowSteerPending = false;
    }

    if (usedTemporaryFib)
      ++m_counters.temporaryFibInterestTransmissions;
    else
      ++m_counters.flowTableFibInterestTransmissions;
    sendInterestToPort(port, *interest);
  }
}

void ForwardingEngine::processStatefulData(uint16_t inPort, std::shared_ptr<::ndn::Data> data,
                                           PacketHeader header)
{
  NS_LOG_DEBUG("ForwardingEngine stateful Data port=" << inPort << " name=" << data->getName());
  ++m_counters.dataReceived;

  uint8_t hopCount = header.getSwitchId();
  ++hopCount;

  data->setTag(std::make_shared<::ndn::lp::HopCountTag>(hopCount));
  header.setSwitchId(hopCount);

  Simulator::Schedule(m_statefulDelay, &ForwardingEngine::processDataAfterDelay, this, inPort, data,
                      header);
}

void ForwardingEngine::processDataAfterDelay(uint16_t inPort, std::shared_ptr<::ndn::Data> data,
                                             PacketHeader header)
{
  ::ndn::Name name = data->getName();

  const bool isTrace = isTraceDataPacket(*data);
  ::ndn::time::microseconds routeLifetime = ::ndn::time::microseconds::zero();
  if (isTrace)
  {
    // The saved TI supplies KITE's route lease. Read it before Data consumes
    // the PIT record; plain TD content does not encode this duration.
    for (const auto& pending : m_pit.findByPrefix(name))
    {
      if (pending->getName() == name)
      {
        routeLifetime = ::ndn::time::duration_cast<::ndn::time::microseconds>(
            pending->getInterestLifetime());
        break;
      }
    }
  }

  const std::set<uint16_t> returnPorts = m_pit.takeDataMatchPorts(name);

  if (returnPorts.empty())
  {
    NS_LOG_DEBUG("  PIT miss, drop unsolicited Data");
    return;
  }

  if (isTrace)
  {
    if (routeLifetime <= ::ndn::time::microseconds::zero())
    {
      ++m_counters.malformedPackets;
      return;
    }
    handleTraceData(inPort, *data, returnPorts, routeLifetime);
  }

  if (!isTrace)
  {
    m_cs.insert(*data);
  }

  header.computeNameHashes(name);
  header.setType(PacketType::DATA);
  header.setOutPorts(StatefulForwarder::encodeOutPortsMask(returnPorts));

  PipelineResult pass2 = m_statefulModule.getSwitch().processPacket(header);

  if (pass2.dropped || pass2.outputPorts.empty())
  {
    NS_LOG_DEBUG("  pass2: Data no output ports");
    return;
  }

  for (uint16_t port : pass2.outputPorts)
  {
    sendDataToPort(port, *data);
  }
}

void ForwardingEngine::handleTraceData(uint16_t inPort, const ::ndn::Data& data,
                                       const std::set<uint16_t>& returnPorts,
                                       ::ndn::time::microseconds lifetime)
{
  validateRouteLifetime(MicroSeconds(lifetime.count()));
  uint64_t seq = getTraceSequence(data);
  ::ndn::Name prefix = getTraceDataPrefix(data);
  if (prefix.empty())
    return;

  ++m_counters.traceUpdatesHandled;

  if (m_experimentTelemetryEnabled)
  {
    std::cerr << "[TRACE_TD] t=" << std::fixed << std::setprecision(9)
              << Simulator::Now().GetSeconds() << " nodeId=" << m_nodeId << " seq=" << seq
              << " prefix=" << prefix << std::endl;
  }

  for (uint16_t port : returnPorts)
  {
    bool updated = m_statefulModule.applyTraceUpdate(prefix, port, seq, lifetime);
    if (m_experimentTelemetryEnabled && updated)
    {
      m_tempSteerPending = true;
      m_tempSteerSeq = seq;
      m_tempSteerPrefix = prefix;
    }
    if (updated && m_enableInterestReforwarding)
    {
      pullPendingInterests(prefix, port);
    }
  }
}

void ForwardingEngine::pullPendingInterests(const ::ndn::Name& prefix, uint16_t newPort)
{
  const int MAX_PULL_COUNT = m_interestReforwardingLimit;

  ++m_counters.pullInvocations;

  ::ndn::Name mutablePrefix(prefix);
  std::vector<std::shared_ptr<const ::ndn::Interest>> pending = m_pit.findByPrefix(mutablePrefix);

  int pulled = static_cast<int>(pending.size());
  if (pulled > MAX_PULL_COUNT)
    pulled = MAX_PULL_COUNT;

  std::cerr << "[STATIM_PULL] t=" << Simulator::Now().GetMilliSeconds() << "ms prefix=" << prefix
            << " newPort=" << newPort << " pit_matched=" << pending.size() << " pulled=" << pulled
            << std::endl;

  int i = 0;
  for (auto& pendingInterest : pending)
  {
    if (i >= MAX_PULL_COUNT)
      break;

    auto retx = std::make_shared<::ndn::Interest>(*pendingInterest);
    retx->refreshNonce();

    ++m_counters.pendingPullsScheduled;
    Simulator::Schedule(MilliSeconds(0), &ForwardingEngine::doDelayedPullSend, this, retx, newPort);
    ++i;
  }
}

bool ForwardingEngine::onFlowTableFlowMod(const ControlMessage& message)
{
  if (message.isExpiredAt(Simulator::Now()))
    return false;
  ++m_counters.flowTableUpdatesApplied;

  if (m_experimentTelemetryEnabled)
  {
    m_flowSteerPending = true;
    m_flowSteerSeq = message.seq;
    m_flowSteerPrefix = message.prefix;
  }

  std::cerr << "[FLOWMOD] t=" << Simulator::Now().GetSeconds() << " prefix=" << message.prefix
            << " port=" << message.outputPort << " prefixLen=" << message.prefixLen
            << " seq=" << message.seq << std::endl;

  if (m_experimentTelemetryEnabled)
  {
    std::cerr << "[FLOWMOD_TELEMETRY] t=" << std::fixed << std::setprecision(9)
              << Simulator::Now().GetSeconds() << " nodeId=" << m_nodeId << " seq=" << message.seq
              << " prefix=" << message.prefix << std::endl;
  }

  m_statefulModule.getSwitch().installFibEntry(message.nameHashes, message.outputPort,
                                               message.routeExpiry);
  return true;
}

void ForwardingEngine::doDelayedPullSend(std::shared_ptr<::ndn::Interest> interest, uint16_t port)
{
  ++m_counters.pendingPullsSent;
  sendInterestToPort(port, *interest);
}

} // namespace statim
} // namespace ndn
} // namespace ns3
