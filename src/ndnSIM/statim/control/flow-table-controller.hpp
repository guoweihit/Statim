// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#ifndef STATIM_CENTRAL_CONTROLLER_HPP
#define STATIM_CENTRAL_CONTROLLER_HPP

#include "ns3/ndnSIM/statim/control/control-message.hpp"
#include "ns3/ptr.h"
#include "ns3/random-variable-stream.h"
#include "ns3/simulator.h"

#include <functional>
#include <map>

namespace ns3
{
namespace ndn
{
namespace statim
{

class FlowTableController
{
public:
  struct Counters
  {
    uint64_t packetIns;
    uint64_t packetInsDelivered;
    uint64_t packetInsDropped;
    uint64_t duplicatePacketIns;
    uint64_t stalePacketInsRejected;
    uint64_t conflictingPacketInsRejected;
    uint64_t flowModAttempts;
    uint64_t flowModsApplied;
    uint64_t flowModsDropped;
    uint64_t flowModsNoApplySink;
    uint64_t staleFlowModEventsSuppressed;
    uint64_t duplicateFlowModEventsSuppressed;
    uint64_t syncAckAttempts;
    uint64_t syncAcksSent;
    uint64_t syncAcksDropped;
    uint64_t idempotentAckReplays;
    uint64_t expiredPacketInsRejected;
    uint64_t expiredFlowModsRejected;
    uint64_t expiredSyncAcksSuppressed;
    uint64_t flowModsRejectedBySink;

    Counters()
        : packetIns(0), packetInsDelivered(0), packetInsDropped(0), duplicatePacketIns(0),
          stalePacketInsRejected(0), conflictingPacketInsRejected(0), flowModAttempts(0),
          flowModsApplied(0), flowModsDropped(0), flowModsNoApplySink(0),
          staleFlowModEventsSuppressed(0), duplicateFlowModEventsSuppressed(0), syncAckAttempts(0),
          syncAcksSent(0), syncAcksDropped(0), idempotentAckReplays(0),
          expiredPacketInsRejected(0), expiredFlowModsRejected(0),
          expiredSyncAcksSuppressed(0), flowModsRejectedBySink(0)
    {
    }
  };

  struct DelayProfile
  {
    Time controlExtra;
    Time flowModExtra;
    Time syncAckExtra;

    DelayProfile(Time control = Seconds(0), Time flowMod = Seconds(0), Time syncAck = Seconds(0))
        : controlExtra(control), flowModExtra(flowMod), syncAckExtra(syncAck)
    {
    }
  };

  // True reports that the route became visible; rejected installation produces
  // no success notification and no applied-generation record.
  typedef std::function<bool(const ControlMessage& message)> FlowModApplyCallback;
  typedef std::function<void(const ControlMessage& message)> SyncAckCallback;

  FlowTableController();

  void setControllerDelay(Time delay) { m_controllerDelay = delay; }

  Time getControllerDelay() const { return m_controllerDelay; }

  void setFlowModApplyCallback(FlowModApplyCallback cb) { m_flowModApplyCb = cb; }

  void setSyncAckCallback(SyncAckCallback cb) { m_syncAckCb = cb; }

  // Enable per-prefix generation checks and idempotent acknowledgement replay.
  void setGenerationGuardEnabled(bool enabled) { m_generationGuardEnabled = enabled; }

  bool isGenerationGuardEnabled() const { return m_generationGuardEnabled; }

  // Independent event-loss probabilities for control requests, flow-table
  // updates, and completion notices in the simulator. Each defaults to zero.
  void setControlLossRate(double p) { m_controlLossRate = p; }

  void setFlowModLossRate(double p) { m_flowModLossRate = p; }

  void setSyncAckLossRate(double p) { m_syncAckLossRate = p; }

  // Deterministic first-N drops make regression cases independent of chance.
  void setControlDropFirstN(uint32_t n) { m_controlDropFirstN = n; }

  void setFlowModDropFirstN(uint32_t n) { m_flowModDropFirstN = n; }

  void setSyncAckDropFirstN(uint32_t n) { m_syncAckDropFirstN = n; }

  // Extra per-leg delay and symmetric jitter. controllerDelay is the base
  // delay from accepted PacketIn to the FlowMod attempt.
  void setControlLegDelay(Time delay) { m_controlLegDelay = delay; }

  void setFlowModLegDelay(Time delay) { m_flowModLegDelay = delay; }

  void setSyncAckLegDelay(Time delay) { m_syncAckLegDelay = delay; }

  void setControlLegJitter(Time jitter) { m_controlLegJitter = jitter; }

  void setFlowModLegJitter(Time jitter) { m_flowModLegJitter = jitter; }

  void setSyncAckLegJitter(Time jitter) { m_syncAckLegJitter = jitter; }

  // A deterministic generation-specific extra delay is useful for forcing a
  // known reordering in regression experiments.
  void setGenerationDelayProfile(uint64_t seq, const DelayProfile& profile)
  {
    m_delayProfiles[seq] = profile;
  }

  void clearGenerationDelayProfiles() { m_delayProfiles.clear(); }

  int64_t assignStreams(int64_t firstStream);

  void submitFlowUpdate(const ControlMessage& message);

  const Counters& getCounters() const { return m_counters; }

  bool getHighestAccepted(const ::ndn::Name& prefix, uint64_t& seq) const;

  bool getHighestApplied(const ::ndn::Name& prefix, uint64_t& seq, uint16_t& outputPort) const;

private:
  struct PrefixState
  {
    bool hasAccepted;
    uint64_t highestAccepted;
    ControlMessage acceptedMessage;
    bool hasApplied;
    uint64_t highestApplied;
    ControlMessage appliedMessage;

    PrefixState() : hasAccepted(false), highestAccepted(0), hasApplied(false), highestApplied(0) {}
  };

  void acceptControl(const ControlMessage& message);

  void attemptFlowMod(const ControlMessage& message);

  void scheduleSyncAck(const ControlMessage& message, bool replay);

  void deliverSyncAck(const ControlMessage& message);

  bool shouldDrop(double probability, uint32_t& deterministicBudget,
                  Ptr<UniformRandomVariable>& random);

  Time sampleDelay(Time base, Time jitter, Ptr<UniformRandomVariable>& random);

  static void ensureRandom(Ptr<UniformRandomVariable>& random);

  DelayProfile getDelayProfile(uint64_t seq) const;

  static bool sameUpdate(const ControlMessage& lhs, const ControlMessage& rhs);

  Time m_controllerDelay;
  FlowModApplyCallback m_flowModApplyCb;
  SyncAckCallback m_syncAckCb;
  Counters m_counters;
  bool m_generationGuardEnabled;
  double m_controlLossRate;
  double m_flowModLossRate;
  double m_syncAckLossRate;
  uint32_t m_controlDropFirstN;
  uint32_t m_flowModDropFirstN;
  uint32_t m_syncAckDropFirstN;
  Time m_controlLegDelay;
  Time m_flowModLegDelay;
  Time m_syncAckLegDelay;
  Time m_controlLegJitter;
  Time m_flowModLegJitter;
  Time m_syncAckLegJitter;
  Ptr<UniformRandomVariable> m_controlLossRand;
  Ptr<UniformRandomVariable> m_flowModLossRand;
  Ptr<UniformRandomVariable> m_syncAckLossRand;
  Ptr<UniformRandomVariable> m_controlDelayRand;
  Ptr<UniformRandomVariable> m_flowModDelayRand;
  Ptr<UniformRandomVariable> m_syncAckDelayRand;
  std::map<::ndn::Name, PrefixState> m_prefixStates;
  std::map<uint64_t, DelayProfile> m_delayProfiles;
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_CENTRAL_CONTROLLER_HPP
