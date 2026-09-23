// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/ndnSIM/statim/control/flow-table-controller.hpp"

#include <algorithm>

namespace ns3
{
namespace ndn
{
namespace statim
{

FlowTableController::FlowTableController()
    : m_controllerDelay(Seconds(1.0)), m_generationGuardEnabled(false), m_controlLossRate(0.0),
      m_flowModLossRate(0.0), m_syncAckLossRate(0.0), m_controlDropFirstN(0),
      m_flowModDropFirstN(0), m_syncAckDropFirstN(0), m_controlLegDelay(Seconds(0)),
      m_flowModLegDelay(Seconds(0)), m_syncAckLegDelay(Seconds(0)), m_controlLegJitter(Seconds(0)),
      m_flowModLegJitter(Seconds(0)), m_syncAckLegJitter(Seconds(0)),
      m_syncAckLossRand(CreateObject<UniformRandomVariable>())
{
}

void FlowTableController::ensureRandom(Ptr<UniformRandomVariable>& random)
{
  if (random == nullptr)
  {
    random = CreateObject<UniformRandomVariable>();
  }
}

int64_t FlowTableController::assignStreams(int64_t firstStream)
{
  ensureRandom(m_controlLossRand);
  ensureRandom(m_flowModLossRand);
  ensureRandom(m_syncAckLossRand);
  ensureRandom(m_controlDelayRand);
  ensureRandom(m_flowModDelayRand);
  ensureRandom(m_syncAckDelayRand);
  m_controlLossRand->SetStream(firstStream + 0);
  m_flowModLossRand->SetStream(firstStream + 1);
  m_syncAckLossRand->SetStream(firstStream + 2);
  m_controlDelayRand->SetStream(firstStream + 3);
  m_flowModDelayRand->SetStream(firstStream + 4);
  m_syncAckDelayRand->SetStream(firstStream + 5);
  return 6;
}

bool FlowTableController::shouldDrop(double probability, uint32_t& deterministicBudget,
                                     Ptr<UniformRandomVariable>& random)
{
  if (deterministicBudget > 0)
  {
    --deterministicBudget;
    return true;
  }
  if (probability <= 0.0)
  {
    return false;
  }
  if (probability >= 1.0)
  {
    return true;
  }
  ensureRandom(random);
  return random->GetValue(0.0, 1.0) < probability;
}

Time FlowTableController::sampleDelay(Time base, Time jitter, Ptr<UniformRandomVariable>& random)
{
  double seconds = base.GetSeconds();
  double width = std::max(0.0, jitter.GetSeconds());
  if (width > 0.0)
  {
    ensureRandom(random);
    seconds += random->GetValue(-width, width);
  }
  return Seconds(std::max(0.0, seconds));
}

FlowTableController::DelayProfile FlowTableController::getDelayProfile(uint64_t seq) const
{
  auto it = m_delayProfiles.find(seq);
  return it == m_delayProfiles.end() ? DelayProfile() : it->second;
}

bool FlowTableController::sameUpdate(const ControlMessage& lhs, const ControlMessage& rhs)
{
  return lhs.prefix == rhs.prefix && lhs.seq == rhs.seq && lhs.prefixLen == rhs.prefixLen &&
         lhs.outputPort == rhs.outputPort && lhs.nameHashes == rhs.nameHashes &&
         lhs.routeExpiry == rhs.routeExpiry;
}

// With zero control-leg loss and delay, acceptControl runs synchronously.
// It schedules the FlowMod attempt after controllerDelay plus any configured
// FlowMod-leg delay and jitter.
void FlowTableController::submitFlowUpdate(const ControlMessage& message)
{
  ++m_counters.packetIns;
  if (shouldDrop(m_controlLossRate, m_controlDropFirstN, m_controlLossRand))
  {
    ++m_counters.packetInsDropped;
    return;
  }

  DelayProfile profile = getDelayProfile(message.seq);
  Time delay =
      sampleDelay(m_controlLegDelay + profile.controlExtra, m_controlLegJitter, m_controlDelayRand);
  if (delay <= Seconds(0))
  {
    acceptControl(message);
  }
  else
  {
    Simulator::Schedule(delay, &FlowTableController::acceptControl, this, message);
  }
}

void FlowTableController::acceptControl(const ControlMessage& message)
{
  ++m_counters.packetInsDelivered;

  if (message.isExpiredAt(Simulator::Now()))
  {
    ++m_counters.expiredPacketInsRejected;
    return;
  }

  if (m_generationGuardEnabled)
  {
    PrefixState& state = m_prefixStates[message.prefix];
    if (state.hasAccepted && message.seq < state.highestAccepted)
    {
      ++m_counters.stalePacketInsRejected;
      return;
    }

    if (state.hasAccepted && message.seq == state.highestAccepted)
    {
      ++m_counters.duplicatePacketIns;
      if (!sameUpdate(message, state.acceptedMessage))
      {
        ++m_counters.conflictingPacketInsRejected;
        return;
      }
      if (state.hasApplied && state.highestApplied == message.seq)
      {
        scheduleSyncAck(message, true);
        return;
      }
      // Schedule another application attempt for this accepted, pending
      // generation so a retry can recover a lost FlowMod.
    }
    else
    {
      state.hasAccepted = true;
      state.highestAccepted = message.seq;
      state.acceptedMessage = message;
    }
  }

  DelayProfile profile = getDelayProfile(message.seq);
  Time delay = sampleDelay(m_controllerDelay + m_flowModLegDelay + profile.flowModExtra,
                           m_flowModLegJitter, m_flowModDelayRand);
  Simulator::Schedule(delay, &FlowTableController::attemptFlowMod, this, message);
}

void FlowTableController::attemptFlowMod(const ControlMessage& message)
{
  ++m_counters.flowModAttempts;

  if (message.isExpiredAt(Simulator::Now()))
  {
    ++m_counters.expiredFlowModsRejected;
    return;
  }

  if (m_generationGuardEnabled)
  {
    auto it = m_prefixStates.find(message.prefix);
    if (it == m_prefixStates.end() || !it->second.hasAccepted ||
        message.seq < it->second.highestAccepted)
    {
      ++m_counters.staleFlowModEventsSuppressed;
      return;
    }
    if (it->second.hasApplied && it->second.highestApplied == message.seq)
    {
      ++m_counters.duplicateFlowModEventsSuppressed;
      scheduleSyncAck(message, true);
      return;
    }
  }

  if (shouldDrop(m_flowModLossRate, m_flowModDropFirstN, m_flowModLossRand))
  {
    ++m_counters.flowModsDropped;
    return;
  }

  if (!m_flowModApplyCb)
  {
    ++m_counters.flowModsNoApplySink;
    return;
  }
  ControlMessage flowMod = message;
  flowMod.type = ControlMessageType::FLOW_MOD;
  if (!m_flowModApplyCb(flowMod))
  {
    ++m_counters.flowModsRejectedBySink;
    return;
  }
  ++m_counters.flowModsApplied;

  if (m_generationGuardEnabled)
  {
    PrefixState& state = m_prefixStates[message.prefix];
    state.hasApplied = true;
    state.highestApplied = message.seq;
    state.appliedMessage = message;
  }

  // Schedule completion after the FlowMod application callback has returned.
  scheduleSyncAck(message, false);
}

void FlowTableController::scheduleSyncAck(const ControlMessage& message, bool replay)
{
  if (replay)
  {
    ++m_counters.idempotentAckReplays;
  }
  DelayProfile profile = getDelayProfile(message.seq);
  Time delay =
      sampleDelay(m_syncAckLegDelay + profile.syncAckExtra, m_syncAckLegJitter, m_syncAckDelayRand);
  if (delay <= Seconds(0))
  {
    deliverSyncAck(message);
  }
  else
  {
    Simulator::Schedule(delay, &FlowTableController::deliverSyncAck, this, message);
  }
}

void FlowTableController::deliverSyncAck(const ControlMessage& message)
{
  ++m_counters.syncAckAttempts;
  if (message.isExpiredAt(Simulator::Now()))
  {
    ++m_counters.expiredSyncAcksSuppressed;
    return;
  }
  if (shouldDrop(m_syncAckLossRate, m_syncAckDropFirstN, m_syncAckLossRand))
  {
    ++m_counters.syncAcksDropped;
    return;
  }

  if (m_syncAckCb)
  {
    ControlMessage ack = message;
    ack.type = ControlMessageType::SYNC_ACK;
    m_syncAckCb(ack);
  }
  ++m_counters.syncAcksSent;
}

bool FlowTableController::getHighestAccepted(const ::ndn::Name& prefix, uint64_t& seq) const
{
  auto it = m_prefixStates.find(prefix);
  if (it == m_prefixStates.end() || !it->second.hasAccepted)
  {
    return false;
  }
  seq = it->second.highestAccepted;
  return true;
}

bool FlowTableController::getHighestApplied(const ::ndn::Name& prefix, uint64_t& seq,
                                            uint16_t& outputPort) const
{
  auto it = m_prefixStates.find(prefix);
  if (it == m_prefixStates.end() || !it->second.hasApplied)
  {
    return false;
  }
  seq = it->second.highestApplied;
  outputPort = it->second.appliedMessage.outputPort;
  return true;
}

} // namespace statim
} // namespace ndn
} // namespace ns3
