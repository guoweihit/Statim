// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#ifndef STATIM_ENGINE_HPP
#define STATIM_ENGINE_HPP

#include "ns3/ndnSIM/statim/packet-utils.hpp"
#include "ns3/ndnSIM/statim/port-table.hpp"

#include "ns3/ndnSIM/statim/control/flow-table-controller.hpp"
#include "ns3/ndnSIM/statim/packet-header.hpp"
#include "ns3/ndnSIM/statim/state/pending-interest-table.hpp"
#include "ns3/ndnSIM/statim/stateful/content-store.hpp"
#include "ns3/ndnSIM/statim/stateful/stateful-forwarder.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/interest.hpp>

#include "ns3/log.h"
#include "ns3/net-device.h"
#include "ns3/node.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

#include <functional>
#include <memory>
#include <set>

namespace ns3
{
namespace ndn
{
namespace statim
{

class ForwardingEngine
{
public:
  struct Counters
  {
    uint64_t interestsReceived = 0;
    uint64_t dataReceived = 0;
    // Count Trace Interest visits separately from ordinary Interests so an
    // experiment can report the path actually traversed.
    uint64_t traceInterestsHandled = 0;
    uint64_t traceUpdatesHandled = 0;
    uint64_t flowTableUpdatesApplied = 0;
    // FIB-path Interest sends (including TI), classified by the FIB that chose
    // the egress. One multicast Interest contributes once per output port.
    // Direct sends of remembered Interests have their own pendingPullsSent count.
    uint64_t temporaryFibInterestTransmissions = 0;
    uint64_t flowTableFibInterestTransmissions = 0;
    uint64_t statimLookupMisses = 0;
    uint64_t noRouteRejections = 0;
    uint64_t pendingPullsScheduled = 0;
    uint64_t pendingPullsSent = 0;
    uint64_t pullInvocations = 0;
    uint64_t csHits = 0;
    uint64_t pitDuplicates = 0;
    uint64_t malformedPackets = 0;
  };

  ForwardingEngine();

  void onReceiveRawPacket(uint16_t inPort, Ptr<const Packet> packet);

  void sendToPort(uint16_t outPort, Ptr<Packet> packet);

  // ---- Component accessors ----

  PortTable& getPortTable() { return m_portTable; }
  const PortTable& getPortTable() const { return m_portTable; }

  SwitchPipeline& getSwitch() { return m_statefulModule.getSwitch(); }

  StatefulForwarder& getStatefulModule() { return m_statefulModule; }
  const StatefulForwarder& getStatefulModule() const { return m_statefulModule; }

  size_t getTemporaryFibSize() const { return m_statefulModule.getTemporaryFibSize(); }

  const FlowTableController& getCentralController() const { return m_centralController; }

  void setStatefulDelay(Time delay) { m_statefulDelay = delay; }
  // Maximum pending Interests resent per pull invocation after a trace update.
  void setInterestReforwardingLimit(int n) { m_interestReforwardingLimit = n; }
  int getInterestReforwardingLimit() const { return m_interestReforwardingLimit; }
  void setControllerDelay(Time delay);
  void setEnableInterestReforwarding(bool v)
  {
    m_enableInterestReforwarding = v;
    m_statefulModule.setEnableInterestReforwarding(v);
  }
  void setEnableTemporaryFib(bool v) { m_enableTemporaryFib = v; }

  // Compatibility setters validate one common configured route lifetime.
  // KITE updates obtain that lifetime from their saved Trace Interest.
  void setFlowTableFibEntryLifetime(Time lt);
  void setTemporaryFibEntryLifetime(Time lt);

  void setControllerRetryTimeout(Time t) { m_statefulModule.getTemporaryController().setControllerRetryTimeout(t); }
  void setControllerRetryLimit(int n)
  {
    m_statefulModule.getTemporaryController().setControllerRetryLimit(n);
  }
  void setGenerationGuardEnabled(bool enabled)
  {
    m_statefulModule.getTemporaryController().setGenerationGuardEnabled(enabled);
    m_centralController.setGenerationGuardEnabled(enabled);
  }
  void setControlLossRate(double p) { m_centralController.setControlLossRate(p); }
  void setFlowModLossRate(double p) { m_centralController.setFlowModLossRate(p); }
  void setSyncAckLossRate(double p) { m_centralController.setSyncAckLossRate(p); }
  void setControllerLegDelays(Time control, Time flowMod, Time syncAck)
  {
    m_centralController.setControlLegDelay(control);
    m_centralController.setFlowModLegDelay(flowMod);
    m_centralController.setSyncAckLegDelay(syncAck);
  }
  void setControllerLegJitters(Time control, Time flowMod, Time syncAck)
  {
    m_centralController.setControlLegJitter(control);
    m_centralController.setFlowModLegJitter(flowMod);
    m_centralController.setSyncAckLegJitter(syncAck);
  }
  int64_t assignControllerStreams(int64_t firstStream)
  {
    return m_centralController.assignStreams(firstStream);
  }

  // Stable node identity attached to experiment telemetry records.
  void setNodeId(uint32_t id) { m_nodeId = id; }
  uint32_t getNodeId() const { return m_nodeId; }

  void setExperimentTelemetryEnabled(bool enabled)
  {
    m_experimentTelemetryEnabled = enabled;
    if (!enabled)
    {
      m_tempSteerPending = false;
      m_flowSteerPending = false;
    }
  }
  bool isExperimentTelemetryEnabled() const { return m_experimentTelemetryEnabled; }

  // ---- Counters ----

  const Counters& getCounters() const { return m_counters; }

private:
  void processStatefulInterest(uint16_t inPort, std::shared_ptr<::ndn::Interest> interest,
                               PacketHeader header);

  void processStatefulData(uint16_t inPort, std::shared_ptr<::ndn::Data> data, PacketHeader header);

  void processInterestAfterDelay(uint16_t inPort, std::shared_ptr<::ndn::Interest> interest,
                                 PacketHeader header);

  void processDataAfterDelay(uint16_t inPort, std::shared_ptr<::ndn::Data> data,
                             PacketHeader header);

  void handleTraceData(uint16_t inPort, const ::ndn::Data& data,
                       const std::set<uint16_t>& returnPorts, ::ndn::time::microseconds lifetime);

  void pullPendingInterests(const ::ndn::Name& prefix, uint16_t newPort);

  bool onFlowTableFlowMod(const ControlMessage& message);

  void validateRouteLifetime(Time lifetime) const;

  void sendInterestToPort(uint16_t outPort, const ::ndn::Interest& interest);

  void sendDataToPort(uint16_t outPort, const ::ndn::Data& data);

  void doDelayedPullSend(std::shared_ptr<::ndn::Interest> interest, uint16_t port);

private:
  PortTable m_portTable;

  ::ns3::ndn::statim::PendingInterestTable m_pit;
  ContentStore m_cs;

  StatefulForwarder m_statefulModule;
  FlowTableController m_centralController;

  Time m_statefulDelay;
  Time m_configuredFlowTableLifetime;
  Time m_configuredTemporaryLifetime;
  bool m_enableTemporaryFib;
  bool m_enableInterestReforwarding;
  int m_interestReforwardingLimit;
  uint32_t m_nodeId;
  bool m_experimentTelemetryEnabled;

  // A trace update and its FlowMod each arm one first-use marker.  The first
  // matching ordinary Interest consumes the marker and emits a telemetry
  // line; forwarding behavior and ordering are unchanged.
  bool m_tempSteerPending;
  bool m_flowSteerPending;
  uint64_t m_tempSteerSeq;
  uint64_t m_flowSteerSeq;
  ::ndn::Name m_tempSteerPrefix;
  ::ndn::Name m_flowSteerPrefix;

  Counters m_counters;
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_ENGINE_HPP
