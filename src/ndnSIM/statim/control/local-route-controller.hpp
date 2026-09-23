// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#ifndef STATIM_TEMPORARY_CONTROLLER_HPP
#define STATIM_TEMPORARY_CONTROLLER_HPP

#include "ns3/ndnSIM/statim/control/control-message.hpp"
#include "ns3/ndnSIM/statim/state/temporary-forwarding-table.hpp"

#include "ns3/event-id.h"

#include <functional>
#include <map>

namespace ns3
{
namespace ndn
{
namespace statim
{

class LocalRouteController
{
public:
  struct Counters
  {
    uint64_t tempUpdates;
    uint64_t controlPackets;
    uint64_t syncCompletions;
    uint64_t staleTraceUpdatesRejected;
    uint64_t staleSyncAcksRejected;
    uint64_t noPendingSyncAcksRejected;
    uint64_t controllerRetries;   // repeated data-plane-initiated control requests
    uint64_t temporaryRouteWithdrawals; // entry withdrawn after the bounded retries failed
    uint64_t expiredSyncAcksRejected;

    Counters()
        : tempUpdates(0), controlPackets(0), syncCompletions(0), staleTraceUpdatesRejected(0),
          staleSyncAcksRejected(0), noPendingSyncAcksRejected(0), controllerRetries(0), temporaryRouteWithdrawals(0),
          expiredSyncAcksRejected(0)
    {
    }
  };

  typedef std::function<void(const ControlMessage& message)> FlowTableSyncRequestCallback;
  typedef std::function<void(const ::ndn::Name& prefix)> SyncCompleteCallback;

  LocalRouteController();

  ::ns3::ndn::statim::TemporaryForwardingTable& getTemporaryFib() { return m_temporaryFib; }

  const ::ns3::ndn::statim::TemporaryForwardingTable& getTemporaryFib() const
  {
    return m_temporaryFib;
  }

  size_t fibSize() const;

  size_t pendingRequestCount() const { return m_pendingControllerRequests.size(); }

  void setControllerDelay(Time delay) { m_controllerDelay = delay; }

  Time getControllerDelay() const { return m_controllerDelay; }

  // A configured lease is a default for synthetic calls and must agree with
  // an explicitly supplied routing-protocol lease. It is separate from retries.
  void setEntryLifetime(Time lifetime);

  // Set the reconciliation timeout. Each timeout retries the pending request;
  // once the retry budget is exhausted, withdraw temporary state and log failure.
  // A zero reconciliation timeout disables retries.
  void setControllerRetryTimeout(Time t) { m_controllerRetryTimeout = t; }

  void setControllerRetryLimit(int n) { m_controllerRetryLimit = n; }

  // Retain the latest accepted sequence number per prefix after SyncAck;
  // subsequent Trace admission requires a newer sequence number.
  void setGenerationGuardEnabled(bool enabled) { m_generationGuardEnabled = enabled; }

  bool isGenerationGuardEnabled() const { return m_generationGuardEnabled; }

  bool getHighestSeenSeq(const ::ndn::Name& prefix, uint64_t& seq) const;

  void setFlowTableSyncRequestCallback(FlowTableSyncRequestCallback cb)
  {
    m_flowTableSyncRequestCb = cb;
  }

  void setSyncCompleteCallback(SyncCompleteCallback cb) { m_syncCompleteCb = cb; }

  // A positive protocol lifetime fixes one deadline at admission. With both
  // ttl and the configured default zero, synthetic updates have no finite lease.
  bool applyTraceUpdate(const ::ndn::Name& prefix, const std::vector<uint8_t>& nameHashes,
                        size_t prefixLen, uint16_t portNumber, uint64_t seq = 0,
                        ::ndn::time::microseconds ttl = ::ndn::time::microseconds::zero());

  bool handleControlResponse(const ControlMessage& message);

  const Counters& getCounters() const { return m_counters; }

private:
  void expireEntryBySeq(const ::ndn::Name& prefix, uint64_t seq);

  void onControllerRetryTimeout(const ::ndn::Name& prefix, uint64_t seq);

  // Retain the request and timer so a timed-out reconciliation can be retried.
  struct PendingControllerRequest
  {
    uint64_t seq;
    int retriesLeft;
    EventId timer;
    ControlMessage msg;
  };

  ::ns3::ndn::statim::TemporaryForwardingTable m_temporaryFib;
  std::map<::ndn::Name, uint64_t> m_seqMap;
  std::map<::ndn::Name, uint64_t> m_highestSeenSeq;
  std::map<::ndn::Name, Time> m_routeExpiries;
  std::map<::ndn::Name, EventId> m_lifetimeTimers;
  std::map<::ndn::Name, PendingControllerRequest> m_pendingControllerRequests;
  Time m_controllerDelay;
  Time m_entryLifetime;
  Time m_controllerRetryTimeout; // 0 = disabled
  int m_controllerRetryLimit;
  bool m_generationGuardEnabled;
  FlowTableSyncRequestCallback m_flowTableSyncRequestCb;
  SyncCompleteCallback m_syncCompleteCb;
  Counters m_counters;
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_TEMPORARY_CONTROLLER_HPP
