// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

// Deterministic controller-leg fault/reordering regression for Statim.
//
// Event-level checks of ordering and bounded retries across control requests,
// flow-table updates, and completion notices in the Statim simulator.

#include "ns3/core-module.h"
#include "ns3/ndnSIM/helper/ndn-stack-helper.hpp"
#include "ns3/ndnSIM/statim/control/flow-table-controller.hpp"
#include "ns3/ndnSIM/statim/control/local-route-controller.hpp"
#include "ns3/ndnSIM/statim/packet-header.hpp"
#include "ns3/ndnSIM/statim/pipeline/switch-pipeline.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ns3
{
namespace ndn
{
namespace statim
{

struct CaseResult
{
  std::string name;
  std::string expectedOutcome;
  bool pass;
  int lookupPort;
  size_t temporaryFibSize;
  bool hasApplied;
  uint64_t appliedSeq;
  uint16_t appliedPort;
  uint64_t flowApplyCallbacks;
  uint64_t ackAccepted;
  uint64_t ackRejected;
  size_t tempProbeHopCount;
  bool tempProbeHasExpectedPort;
  bool tempProbeHasOldPort;
  FlowTableController::Counters central;
  LocalRouteController::Counters temporary;
};

class Fixture
{
public:
  Fixture()
      : prefix("/controller-faults/mobile"), flowApplyCallbacks(0), ackAccepted(0), ackRejected(0),
        tempProbeHopCount(0), tempProbeHasExpectedPort(false), tempProbeHasOldPort(false)
  {
    flowSwitch.initPipeline();
    central.setControllerDelay(MilliSeconds(10));
    central.setGenerationGuardEnabled(true);
    central.assignStreams(3000);
    temporary.setGenerationGuardEnabled(true);
    temporary.setControllerRetryTimeout(MilliSeconds(30));
    temporary.setControllerRetryLimit(1);

    temporary.setFlowTableSyncRequestCallback([this](const ControlMessage& message)
                                              { central.submitFlowUpdate(message); });
    central.setFlowModApplyCallback(
        [this](const ControlMessage& message)
        {
          ++flowApplyCallbacks;
          flowSwitch.installFibEntry(message.nameHashes, message.outputPort, message.routeExpiry);
          return true;
        });
    central.setSyncAckCallback(
        [this](const ControlMessage& message)
        {
          if (temporary.handleControlResponse(message))
          {
            ++ackAccepted;
          }
          else
          {
            ++ackRejected;
          }
        });
  }

  ControlMessage makeMessage(uint64_t seq, uint16_t port, Time expiry = Seconds(0)) const
  {
    PacketHeader header(prefix);
    std::vector<uint8_t> hashes = header.getNameHashes();
    size_t prefixLen = std::min(prefix.size(), static_cast<size_t>(STATIM_NAME_HASH_COUNT));
    return ControlMessage(ControlMessageType::PACKET_IN, hashes, prefixLen, port, prefix, seq,
                          expiry);
  }

  bool trace(uint64_t seq, uint16_t port)
  {
    ControlMessage message = makeMessage(seq, port);
    return temporary.applyTraceUpdate(prefix, message.nameHashes, message.prefixLen, port, seq);
  }

  bool traceWithLifetime(uint64_t seq, uint16_t port, Time lifetime)
  {
    ControlMessage message = makeMessage(seq, port);
    return temporary.applyTraceUpdate(prefix, message.nameHashes, message.prefixLen, port, seq,
                                     ::ndn::time::microseconds(lifetime.GetMicroSeconds()));
  }

  void traceEvent(uint64_t seq, uint16_t port) { trace(seq, port); }

  void probeTempNextHops(uint16_t expectedPort, uint16_t oldPort)
  {
    tempProbeHopCount = 0;
    tempProbeHasExpectedPort = false;
    tempProbeHasOldPort = false;
    auto* entry = temporary.getTemporaryFib().findExact(prefix);
    if (entry == nullptr)
    {
      return;
    }
    for (const auto& hop : entry->getNextHops())
    {
      ++tempProbeHopCount;
      tempProbeHasExpectedPort = tempProbeHasExpectedPort || hop.second.port == expectedPort;
      tempProbeHasOldPort = tempProbeHasOldPort || hop.second.port == oldPort;
    }
  }

  int lookup() const
  {
    ::ndn::Name query(prefix);
    query.append("object");
    PacketHeader header(query);
    header.setType(PacketType::INTEREST);
    header.setOutPorts(0);
    PipelineResult result = flowSwitch.processPacket(header);
    if (result.dropped || result.outputPorts.size() != 1)
    {
      return -1;
    }
    return static_cast<int>(*result.outputPorts.begin());
  }

  CaseResult capture(const std::string& name, const std::string& expected) const
  {
    CaseResult result;
    result.name = name;
    result.expectedOutcome = expected;
    result.pass = false;
    result.lookupPort = lookup();
    result.temporaryFibSize = temporary.fibSize();
    result.hasApplied = central.getHighestApplied(prefix, result.appliedSeq, result.appliedPort);
    if (!result.hasApplied)
    {
      result.appliedSeq = 0;
      result.appliedPort = 0;
    }
    result.flowApplyCallbacks = flowApplyCallbacks;
    result.ackAccepted = ackAccepted;
    result.ackRejected = ackRejected;
    result.tempProbeHopCount = tempProbeHopCount;
    result.tempProbeHasExpectedPort = tempProbeHasExpectedPort;
    result.tempProbeHasOldPort = tempProbeHasOldPort;
    result.central = central.getCounters();
    result.temporary = temporary.getCounters();
    return result;
  }

  FlowTableController central;
  LocalRouteController temporary;
  SwitchPipeline flowSwitch;
  ::ndn::Name prefix;
  uint64_t flowApplyCallbacks;
  uint64_t ackAccepted;
  uint64_t ackRejected;
  size_t tempProbeHopCount;
  bool tempProbeHasExpectedPort;
  bool tempProbeHasOldPort;
};

static void runUntil(Time until)
{
  Simulator::Stop(until);
  Simulator::Run();
}

static CaseResult caseNormal()
{
  LocalRouteController synchronous;
  synchronous.setControllerRetryTimeout(MilliSeconds(30));
  bool synchronousAckAccepted = false;
  synchronous.setFlowTableSyncRequestCallback([&](const ControlMessage& message)
  {
    ControlMessage ack = message;
    ack.type = ControlMessageType::SYNC_ACK;
    synchronousAckAccepted = synchronous.handleControlResponse(ack);
  });
  const ::ndn::Name synchronousPrefix("/controller-faults/synchronous");
  PacketHeader synchronousHeader(synchronousPrefix);
  bool synchronousAccepted = synchronous.applyTraceUpdate(
      synchronousPrefix, synchronousHeader.getNameHashes(), synchronousPrefix.size(), 3, 1,
      ::ndn::time::milliseconds(100));
  bool synchronousClean = synchronousAccepted && synchronousAckAccepted &&
                          synchronous.fibSize() == 0 && synchronous.pendingRequestCount() == 0;

  Fixture f;
  bool accepted = f.trace(1, 3);
  runUntil(MilliSeconds(100));
  CaseResult r = f.capture("normal", "recovered");
  r.pass = synchronousClean && synchronous.getCounters().controllerRetries == 0 &&
           accepted && r.lookupPort == 3 && r.temporaryFibSize == 0 && r.hasApplied &&
           r.appliedSeq == 1 && r.appliedPort == 3 && r.central.flowModsApplied == 1 &&
           r.central.syncAcksSent == 1 && r.ackAccepted == 1;
  Simulator::Destroy();
  return r;
}

static CaseResult caseDuplicateSameSeq()
{
  Fixture f;
  ControlMessage duplicate = f.makeMessage(1, 4);
  bool accepted = f.trace(1, 4);
  Simulator::Schedule(MilliSeconds(1), &FlowTableController::submitFlowUpdate, &f.central,
                      duplicate);
  runUntil(MilliSeconds(100));
  CaseResult r = f.capture("duplicate_same_seq", "idempotent");
  r.pass = accepted && r.lookupPort == 4 && r.flowApplyCallbacks == 1 &&
           r.central.duplicatePacketIns == 1 && r.central.duplicateFlowModEventsSuppressed == 1 &&
           r.central.idempotentAckReplays == 1;
  Simulator::Destroy();
  return r;
}

static CaseResult caseOldControlLate()
{
  Fixture f;
  f.central.setGenerationDelayProfile(
      1, FlowTableController::DelayProfile(MilliSeconds(50), Seconds(0), Seconds(0)));
  bool first = f.trace(1, 5);
  Simulator::Schedule(MilliSeconds(1), &Fixture::traceEvent, &f, static_cast<uint64_t>(2),
                      static_cast<uint16_t>(6));
  Simulator::Schedule(MilliSeconds(2), &Fixture::probeTempNextHops, &f, static_cast<uint16_t>(6),
                      static_cast<uint16_t>(5));
  runUntil(MilliSeconds(120));
  CaseResult r = f.capture("old_control_late", "stale-rejected");
  r.pass = first && r.lookupPort == 6 && r.flowApplyCallbacks == 1 && r.hasApplied &&
           r.appliedSeq == 2 && r.appliedPort == 6 && r.central.stalePacketInsRejected == 1 &&
           r.tempProbeHopCount == 1 && r.tempProbeHasExpectedPort && !r.tempProbeHasOldPort;
  Simulator::Destroy();
  return r;
}

static CaseResult caseOldFlowModLate()
{
  Fixture f;
  f.central.setGenerationDelayProfile(
      1, FlowTableController::DelayProfile(Seconds(0), MilliSeconds(50), Seconds(0)));
  bool first = f.trace(1, 7);
  Simulator::Schedule(MilliSeconds(1), &Fixture::traceEvent, &f, static_cast<uint64_t>(2),
                      static_cast<uint16_t>(8));
  runUntil(MilliSeconds(120));
  CaseResult r = f.capture("old_flowmod_late", "stale-event-suppressed");
  r.pass = first && r.lookupPort == 8 && r.flowApplyCallbacks == 1 && r.hasApplied &&
           r.appliedSeq == 2 && r.appliedPort == 8 && r.central.staleFlowModEventsSuppressed == 1;
  Simulator::Destroy();
  return r;
}

static CaseResult caseControlDropOnce()
{
  Fixture f;
  f.central.setControlDropFirstN(1);
  bool accepted = f.trace(1, 9);
  runUntil(MilliSeconds(150));
  CaseResult r = f.capture("control_drop_once", "retry-recovered");
  r.pass = accepted && r.lookupPort == 9 && r.temporaryFibSize == 0 && r.central.packetInsDropped == 1 &&
           r.temporary.controllerRetries == 1 && r.temporary.temporaryRouteWithdrawals == 0 &&
           r.central.flowModsApplied == 1;
  Simulator::Destroy();
  return r;
}

static CaseResult caseFlowModDropOnce()
{
  Fixture f;
  f.central.setFlowModDropFirstN(1);
  bool accepted = f.trace(1, 10);
  runUntil(MilliSeconds(150));
  CaseResult r = f.capture("flowmod_drop_once", "retry-recovered");
  r.pass = accepted && r.lookupPort == 10 && r.temporaryFibSize == 0 && r.central.flowModsDropped == 1 &&
           r.temporary.controllerRetries == 1 && r.central.flowModsApplied == 1;
  Simulator::Destroy();
  return r;
}

static CaseResult caseAckDropOnce()
{
  Fixture f;
  f.central.setSyncAckDropFirstN(1);
  bool accepted = f.trace(1, 11);
  runUntil(MilliSeconds(150));
  CaseResult r = f.capture("ack_drop_once", "ack-replay-recovered");
  r.pass = accepted && r.lookupPort == 11 && r.temporaryFibSize == 0 && r.central.syncAcksDropped == 1 &&
           r.temporary.controllerRetries == 1 && r.central.flowModsApplied == 1 &&
           r.flowApplyCallbacks == 1 && r.central.idempotentAckReplays == 1;
  Simulator::Destroy();
  return r;
}

static CaseResult caseControlDropAll()
{
  Fixture f;
  f.central.setControlLossRate(1.0);
  bool accepted = f.trace(1, 12);
  runUntil(MilliSeconds(150));
  CaseResult r = f.capture("control_drop_all", "retry-exhausted-withdraw");
  r.pass = accepted && r.lookupPort == -1 && r.temporaryFibSize == 0 && !r.hasApplied &&
           r.temporary.controllerRetries == 1 && r.temporary.temporaryRouteWithdrawals == 1 &&
           r.central.flowModsApplied == 0;
  Simulator::Destroy();
  return r;
}

static CaseResult caseFlowModDropAll()
{
  Fixture f;
  f.central.setFlowModLossRate(1.0);
  bool accepted = f.trace(1, 13);
  runUntil(MilliSeconds(150));
  CaseResult r = f.capture("flowmod_drop_all", "retry-exhausted-withdraw");
  r.pass = accepted && r.lookupPort == -1 && r.temporaryFibSize == 0 && !r.hasApplied &&
           r.temporary.controllerRetries == 1 && r.temporary.temporaryRouteWithdrawals == 1 &&
           r.central.flowModsApplied == 0;
  Simulator::Destroy();
  return r;
}

static CaseResult caseAckDropAll()
{
  Fixture f;
  f.central.setSyncAckLossRate(1.0);
  bool accepted = f.trace(1, 14);
  runUntil(MilliSeconds(150));
  CaseResult r = f.capture("ack_drop_all", "retry-exhausted-flow-present");
  r.pass = accepted && r.lookupPort == 14 && r.temporaryFibSize == 0 && r.hasApplied &&
           r.appliedSeq == 1 && r.temporary.controllerRetries == 1 && r.temporary.temporaryRouteWithdrawals == 1 &&
           r.flowApplyCallbacks == 1;
  Simulator::Destroy();
  return r;
}

static CaseResult caseOldAckRejected()
{
  Fixture f;
  f.central.setGenerationDelayProfile(
      1, FlowTableController::DelayProfile(Seconds(0), Seconds(0), MilliSeconds(60)));
  bool first = f.trace(1, 15);
  Simulator::Schedule(MilliSeconds(20), &Fixture::traceEvent, &f, static_cast<uint64_t>(2),
                      static_cast<uint16_t>(16));
  runUntil(MilliSeconds(150));
  CaseResult r = f.capture("old_ack_late", "local-stale-ack-rejected");
  r.pass = first && r.lookupPort == 16 && r.hasApplied && r.appliedSeq == 2 &&
           r.appliedPort == 16 && r.temporary.staleSyncAcksRejected >= 1 && r.ackRejected >= 1;
  Simulator::Destroy();
  return r;
}

static CaseResult caseConflictingDuplicate()
{
  Fixture f;
  ControlMessage conflict = f.makeMessage(1, 18);
  ControlMessage lifetimeConflict = f.makeMessage(1, 17);
  lifetimeConflict.routeExpiry = Seconds(6);
  bool first = f.trace(1, 17);
  Simulator::Schedule(MilliSeconds(1), &FlowTableController::submitFlowUpdate, &f.central,
                      conflict);
  Simulator::Schedule(MilliSeconds(2), &FlowTableController::submitFlowUpdate, &f.central,
                      lifetimeConflict);
  runUntil(MilliSeconds(100));
  CaseResult r = f.capture("same_seq_conflict", "conflict-rejected");
  r.pass = first && r.lookupPort == 17 && r.flowApplyCallbacks == 1 &&
           r.central.duplicatePacketIns == 2 && r.central.conflictingPacketInsRejected == 2;
  Simulator::Destroy();
  return r;
}

static CaseResult caseSharedRouteExpiry()
{
  Fixture f;
  f.temporary.setControllerRetryTimeout(Seconds(0));
  f.central.setSyncAckLossRate(1);
  f.central.setControllerDelay(MilliSeconds(20));
  bool accepted = f.traceWithLifetime(1, 3, MilliSeconds(100));
  runUntil(MilliSeconds(19));
  bool beforeInstall = f.temporary.getTemporaryFib().findExact(f.prefix) != nullptr && f.lookup() == -1;
  runUntil(MilliSeconds(2));
  bool bothInstalled = f.temporary.getTemporaryFib().findExact(f.prefix) != nullptr && f.lookup() == 3;
  runUntil(MilliSeconds(78));
  bool beforeExpiry = f.temporary.getTemporaryFib().findExact(f.prefix) != nullptr && f.lookup() == 3;
  runUntil(MilliSeconds(1));
  CaseResult r = f.capture("shared_route_expiry", "both-expire-at-local-deadline");
  r.pass = accepted && beforeInstall && bothInstalled && beforeExpiry && r.lookupPort == -1 &&
           r.temporaryFibSize == 0 && f.temporary.getTemporaryFib().findExact(f.prefix) == nullptr &&
           r.central.flowModsApplied == 1 && r.ackAccepted == 0;
  Simulator::Destroy();
  return r;
}

static CaseResult caseExpiredAtInstall()
{
  Fixture f;
  f.temporary.setControllerRetryTimeout(Seconds(0));
  f.central.setControllerDelay(MilliSeconds(100));
  bool accepted = f.traceWithLifetime(1, 3, MilliSeconds(100));
  runUntil(MilliSeconds(110));
  CaseResult r = f.capture("expired_at_install", "expired-flowmod-rejected");
  r.pass = accepted && r.lookupPort == -1 && r.temporaryFibSize == 0 && !r.hasApplied &&
           r.central.expiredFlowModsRejected == 1 && r.central.flowModsApplied == 0 &&
           r.flowApplyCallbacks == 0 && r.central.syncAckAttempts == 0 && r.ackAccepted == 0;
  Simulator::Destroy();
  return r;
}

static CaseResult caseExpiredBeforeControl()
{
  Fixture f;
  f.temporary.setControllerRetryTimeout(Seconds(0));
  f.central.setControlLegDelay(MilliSeconds(150));
  bool accepted = f.traceWithLifetime(1, 3, MilliSeconds(100));
  runUntil(MilliSeconds(170));
  CaseResult r = f.capture("expired_before_control", "expired-request-rejected");
  r.pass = accepted && r.lookupPort == -1 && r.temporaryFibSize == 0 &&
           r.central.expiredPacketInsRejected == 1 && r.central.flowModAttempts == 0 &&
           r.central.syncAckAttempts == 0 && r.ackAccepted == 0;
  Simulator::Destroy();
  return r;
}

static CaseResult caseRetryPreservesExpiry()
{
  Fixture f;
  // Repeated applications without persistent guarding still carry the same
  // immutable deadline; a retry does not obtain a new lease.
  f.temporary.setGenerationGuardEnabled(false);
  f.central.setGenerationGuardEnabled(false);
  f.temporary.setControllerRetryLimit(2);
  f.central.setSyncAckLossRate(1);
  bool accepted = f.traceWithLifetime(1, 3, MilliSeconds(100));
  runUntil(MilliSeconds(99));
  bool presentBeforeDeadline = f.lookup() == 3;
  runUntil(MilliSeconds(1));
  CaseResult r = f.capture("retry_preserves_expiry", "retries-retain-original-deadline");
  r.pass = accepted && presentBeforeDeadline && r.lookupPort == -1 && r.temporaryFibSize == 0 &&
           r.central.flowModsApplied == 3 && r.temporary.controllerRetries == 2 &&
           r.central.syncAcksDropped == 3 && r.ackAccepted == 0;
  Simulator::Destroy();
  return r;
}

static CaseResult caseCompletionAfterExpiry()
{
  Fixture f;
  f.temporary.setControllerRetryTimeout(Seconds(0));
  f.central.setSyncAckLegDelay(MilliSeconds(120));
  bool accepted = f.traceWithLifetime(1, 3, MilliSeconds(100));
  runUntil(MilliSeconds(150));
  CaseResult r = f.capture("completion_after_expiry", "expired-completion-suppressed");
  r.pass = accepted && r.lookupPort == -1 && r.temporaryFibSize == 0 &&
           r.central.flowModsApplied == 1 && r.central.expiredSyncAcksSuppressed == 1 &&
           r.central.syncAcksSent == 0 && r.ackAccepted == 0;
  Simulator::Destroy();
  return r;
}

static CaseResult caseSameSeqExpiryConflict()
{
  Fixture f;
  bool accepted = f.traceWithLifetime(1, 3, MilliSeconds(100));
  auto conflict = f.makeMessage(1, 3, MilliSeconds(200));
  Simulator::Schedule(MilliSeconds(1), &FlowTableController::submitFlowUpdate, &f.central, conflict);
  runUntil(MilliSeconds(20));
  bool present = f.lookup() == 3;
  runUntil(MilliSeconds(80));
  CaseResult r = f.capture("same_seq_expiry_conflict", "changed-deadline-is-payload-conflict");
  r.pass = accepted && present && r.lookupPort == -1 && r.central.flowModsApplied == 1 &&
           r.central.conflictingPacketInsRejected == 1 && r.ackAccepted == 1;
  Simulator::Destroy();
  return r;
}

static CaseResult caseRefreshHasNewExpiry()
{
  Fixture f;
  f.temporary.setControllerRetryTimeout(Seconds(0));
  f.central.setSyncAckLossRate(1);
  bool first = f.traceWithLifetime(1, 3, MilliSeconds(100));
  runUntil(MilliSeconds(50));
  bool second = f.traceWithLifetime(2, 4, MilliSeconds(100));
  runUntil(MilliSeconds(51));
  bool renewed = f.lookup() == 4 && f.temporary.getTemporaryFib().findExact(f.prefix) != nullptr;
  runUntil(MilliSeconds(49));
  CaseResult r = f.capture("refresh_has_new_expiry", "new-generation-renews-both-tables");
  r.pass = first && second && renewed && r.lookupPort == -1 && r.temporaryFibSize == 0 &&
           r.central.flowModsApplied == 2 && r.ackAccepted == 0;
  Simulator::Destroy();
  return r;
}

static CaseResult caseSinkRejectionNoAck()
{
  Fixture f;
  f.temporary.setControllerRetryTimeout(Seconds(0));
  f.central.setFlowModApplyCallback([](const ControlMessage&) { return false; });
  bool accepted = f.traceWithLifetime(1, 3, MilliSeconds(100));
  runUntil(MilliSeconds(110));
  CaseResult r = f.capture("sink_rejection_no_ack", "rejected-install-has-no-success-notice");
  r.pass = accepted && r.lookupPort == -1 && !r.hasApplied && r.central.flowModsApplied == 0 &&
           r.central.flowModsRejectedBySink == 1 && r.central.syncAckAttempts == 0 && r.ackAccepted == 0;
  Simulator::Destroy();
  return r;
}

static void writeCsv(std::ostream& os, const std::vector<CaseResult>& results)
{
  os << "case,expected_outcome,pass,lookup_port,temporary_fib_size,has_applied,"
        "applied_seq,applied_port,packet_in_attempts,control_delivered,"
        "control_dropped,duplicate_control,stale_control,conflicting_control,"
        "flowmod_attempts,flowmod_applied,flowmod_dropped,stale_flowmod,"
        "flowmod_no_sink,duplicate_flowmod,ack_attempts,ack_sent,ack_dropped,ack_replays,"
        "temp_control_packets,controller_retries,temporary_route_withdrawals,stale_trace,"
        "stale_ack,flow_apply_callbacks,ack_accepted,ack_rejected,"
        "temp_probe_hop_count,temp_probe_has_expected_port,temp_probe_has_old_port,"
        "expired_control,expired_flowmod,expired_ack,flowmod_rejected_by_sink\n";
  for (const auto& r : results)
  {
    os << r.name << ',' << r.expectedOutcome << ',' << (r.pass ? 1 : 0) << ',' << r.lookupPort
       << ',' << r.temporaryFibSize << ',' << (r.hasApplied ? 1 : 0) << ',' << r.appliedSeq << ','
       << r.appliedPort << ',' << r.central.packetIns << ',' << r.central.packetInsDelivered << ','
       << r.central.packetInsDropped << ',' << r.central.duplicatePacketIns << ','
       << r.central.stalePacketInsRejected << ',' << r.central.conflictingPacketInsRejected << ','
       << r.central.flowModAttempts << ',' << r.central.flowModsApplied << ','
       << r.central.flowModsDropped << ',' << r.central.staleFlowModEventsSuppressed << ','
       << r.central.flowModsNoApplySink << ',' << r.central.duplicateFlowModEventsSuppressed << ','
       << r.central.syncAckAttempts << ',' << r.central.syncAcksSent << ','
       << r.central.syncAcksDropped << ',' << r.central.idempotentAckReplays << ','
       << r.temporary.controlPackets << ',' << r.temporary.controllerRetries << ','
       << r.temporary.temporaryRouteWithdrawals << ',' << r.temporary.staleTraceUpdatesRejected << ','
       << r.temporary.staleSyncAcksRejected << ',' << r.flowApplyCallbacks << ',' << r.ackAccepted
       << ',' << r.ackRejected << ',' << r.tempProbeHopCount << ','
       << (r.tempProbeHasExpectedPort ? 1 : 0) << ',' << (r.tempProbeHasOldPort ? 1 : 0) << ','
       << r.central.expiredPacketInsRejected << ',' << r.central.expiredFlowModsRejected << ','
       << r.central.expiredSyncAcksSuppressed << ',' << r.central.flowModsRejectedBySink << '\n';
  }
}

int main(int argc, char* argv[])
{
  std::string output;
  CommandLine cmd;
  cmd.AddValue("output", "optional CSV output path", output);
  cmd.Parse(argc, argv);
  ns3::ndn::StackHelper clockSetup;

  std::vector<CaseResult> results;
  results.push_back(caseNormal());
  results.push_back(caseDuplicateSameSeq());
  results.push_back(caseOldControlLate());
  results.push_back(caseOldFlowModLate());
  results.push_back(caseControlDropOnce());
  results.push_back(caseFlowModDropOnce());
  results.push_back(caseAckDropOnce());
  results.push_back(caseControlDropAll());
  results.push_back(caseFlowModDropAll());
  results.push_back(caseAckDropAll());
  results.push_back(caseOldAckRejected());
  results.push_back(caseConflictingDuplicate());
  results.push_back(caseSharedRouteExpiry());
  results.push_back(caseExpiredAtInstall());
  results.push_back(caseExpiredBeforeControl());
  results.push_back(caseRetryPreservesExpiry());
  results.push_back(caseCompletionAfterExpiry());
  results.push_back(caseSameSeqExpiryConflict());
  results.push_back(caseRefreshHasNewExpiry());
  results.push_back(caseSinkRejectionNoAck());

  writeCsv(std::cout, results);
  if (!output.empty())
  {
    std::ofstream file(output.c_str());
    if (!file)
    {
      std::cerr << "cannot open output: " << output << std::endl;
      return 2;
    }
    writeCsv(file, results);
  }

  bool allPassed = true;
  for (const auto& result : results)
  {
    allPassed = allPassed && result.pass;
  }
  return allPassed ? 0 : 1;
}

} // namespace statim
} // namespace ndn
} // namespace ns3

int main(int argc, char* argv[])
{
  return ns3::ndn::statim::main(argc, argv);
}
