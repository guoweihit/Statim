// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/ndnSIM/statim/control/local-route-controller.hpp"
#include "ns3/simulator.h"

#include <stdexcept>

namespace ns3
{
namespace ndn
{
namespace statim
{

LocalRouteController::LocalRouteController()
    : m_controllerDelay(Seconds(1.0)), m_entryLifetime(Seconds(0)), m_controllerRetryTimeout(Seconds(0)),
      m_controllerRetryLimit(3), m_generationGuardEnabled(false)
{
}

size_t LocalRouteController::fibSize() const
{
  return m_seqMap.size();
}

void LocalRouteController::setEntryLifetime(Time lifetime)
{
  if (lifetime < Seconds(0))
    throw std::invalid_argument("Route lifetime must be nonnegative");
  m_entryLifetime = lifetime;
}

bool LocalRouteController::getHighestSeenSeq(const ::ndn::Name& prefix, uint64_t& seq) const
{
  auto it = m_highestSeenSeq.find(prefix);
  if (it == m_highestSeenSeq.end())
  {
    return false;
  }
  seq = it->second;
  return true;
}

bool LocalRouteController::applyTraceUpdate(const ::ndn::Name& prefix,
                                            const std::vector<uint8_t>& nameHashes,
                                            size_t prefixLen, uint16_t portNumber, uint64_t seq,
                                            ::ndn::time::microseconds ttl)
{
  if (ttl < ::ndn::time::microseconds::zero())
    throw std::invalid_argument("Routing-protocol lifetime must be nonnegative");
  const Time suppliedLifetime = MicroSeconds(ttl.count());
  if (suppliedLifetime > Seconds(0) && m_entryLifetime > Seconds(0) &&
      suppliedLifetime != m_entryLifetime)
    throw std::invalid_argument("Configured route lifetime differs from the routing-protocol lifetime");
  const Time lifetime = suppliedLifetime > Seconds(0) ? suppliedLifetime : m_entryLifetime;
  const Time routeExpiry = lifetime > Seconds(0) ? Simulator::Now() + lifetime : Seconds(0);

  // The optional generation guard retains the latest accepted sequence
  // number after SyncAck and rejects delayed older Trace updates.
  if (m_generationGuardEnabled)
  {
    auto highIt = m_highestSeenSeq.find(prefix);
    if (highIt != m_highestSeenSeq.end() && seq <= highIt->second)
    {
      ++m_counters.staleTraceUpdatesRejected;
      return false;
    }
    m_highestSeenSeq[prefix] = seq;
  }

  auto seqIt = m_seqMap.find(prefix);
  if (seqIt != m_seqMap.end() && seq <= seqIt->second)
  {
    ++m_counters.staleTraceUpdatesRejected;
    return false;
  }

  // A generation owns one next-hop decision. Replace the old decision before
  // publishing the new generation; the storage table itself is policy-neutral.
  if (seqIt != m_seqMap.end())
  {
    m_temporaryFib.erase(prefix);
  }
  m_seqMap[prefix] = seq;
  m_routeExpiries[prefix] = routeExpiry;
  const TimePoint tableDeadline = lifetime > Seconds(0)
      ? StateClock::now() + ::ndn::time::nanoseconds(lifetime.GetNanoSeconds())
      : TimePoint::max();
  m_temporaryFib.insertUntil(prefix, portNumber, tableDeadline);

  ++m_counters.tempUpdates;
  auto ltIt = m_lifetimeTimers.find(prefix);
  if (ltIt != m_lifetimeTimers.end())
  {
    Simulator::Cancel(ltIt->second);
    m_lifetimeTimers.erase(ltIt);
  }
  if (lifetime > Seconds(0))
  {
    m_lifetimeTimers[prefix] = Simulator::Schedule(
        lifetime, &LocalRouteController::expireEntryBySeq, this, prefix, seq);
  }
  if (m_flowTableSyncRequestCb)
  {
    ++m_counters.controlPackets;

    ControlMessage msg(ControlMessageType::PACKET_IN, nameHashes, prefixLen, portNumber, prefix,
                       seq, routeExpiry);

    // Replace any pending request for this prefix and cancel its retry timer.
    if (m_controllerRetryTimeout > Seconds(0))
    {
      auto pIt = m_pendingControllerRequests.find(prefix);
      if (pIt != m_pendingControllerRequests.end())
      {
        Simulator::Cancel(pIt->second.timer);
        m_pendingControllerRequests.erase(pIt);
      }
      PendingControllerRequest rec;
      rec.seq = seq;
      rec.retriesLeft = m_controllerRetryLimit;
      rec.msg = msg;
      rec.timer = Simulator::Schedule(m_controllerRetryTimeout, &LocalRouteController::onControllerRetryTimeout, this,
                                      prefix, seq);
      m_pendingControllerRequests[prefix] = rec;
    }
    // Publish pending state before dispatch: a synchronous completion can then
    // cancel and erase the whole transaction, including its retry timer.
    m_flowTableSyncRequestCb(msg);
  }

  return true;
}

bool LocalRouteController::handleControlResponse(const ControlMessage& message)
{
  if (message.type != ControlMessageType::SYNC_ACK)
  {
    return false;
  }
  auto seqIt = m_seqMap.find(message.prefix);
  if (seqIt == m_seqMap.end())
  {
    auto highIt = m_highestSeenSeq.find(message.prefix);
    if (m_generationGuardEnabled && highIt != m_highestSeenSeq.end() &&
        message.seq <= highIt->second)
    {
      ++m_counters.staleSyncAcksRejected;
    }
    else
    {
      ++m_counters.noPendingSyncAcksRejected;
    }
    return false;
  }

  if ((m_generationGuardEnabled && seqIt->second != message.seq) ||
      (!m_generationGuardEnabled && message.seq != 0 && seqIt->second != message.seq))
  {
    ++m_counters.staleSyncAcksRejected;
    return false;
  }

  const Time routeExpiry = m_routeExpiries.at(message.prefix);
  if (routeExpiry > Seconds(0) && Simulator::Now() >= routeExpiry)
  {
    ++m_counters.expiredSyncAcksRejected;
    return false;
  }
  if (message.routeExpiry != routeExpiry)
  {
    ++m_counters.staleSyncAcksRejected;
    return false;
  }

  ++m_counters.syncCompletions;
  m_temporaryFib.erase(message.prefix);
  m_seqMap.erase(seqIt);
  m_routeExpiries.erase(message.prefix);

  auto ltIt = m_lifetimeTimers.find(message.prefix);
  if (ltIt != m_lifetimeTimers.end())
  {
    Simulator::Cancel(ltIt->second);
    m_lifetimeTimers.erase(ltIt);
  }

  auto pIt = m_pendingControllerRequests.find(message.prefix);
  if (pIt != m_pendingControllerRequests.end())
  {
    Simulator::Cancel(pIt->second.timer);
    m_pendingControllerRequests.erase(pIt);
  }
  if (m_syncCompleteCb)
  {
    m_syncCompleteCb(message.prefix);
  }

  return true;
}

void LocalRouteController::expireEntryBySeq(const ::ndn::Name& prefix, uint64_t seq)
{
  auto sIt = m_seqMap.find(prefix);
  if (sIt != m_seqMap.end() && sIt->second == seq)
  {
    m_temporaryFib.erase(prefix);
    m_seqMap.erase(sIt);
    m_routeExpiries.erase(prefix);
    auto pending = m_pendingControllerRequests.find(prefix);
    if (pending != m_pendingControllerRequests.end() && pending->second.seq == seq)
    {
      Simulator::Cancel(pending->second.timer);
      m_pendingControllerRequests.erase(pending);
    }
  }
  m_lifetimeTimers.erase(prefix);
}

// Retry the pending request up to its budget, then withdraw temporary state.
// An installed flow-table route keeps the same logical lease deadline.
// Each retry belongs to the current pending sequence number for its prefix.
void LocalRouteController::onControllerRetryTimeout(const ::ndn::Name& prefix, uint64_t seq)
{
  auto pIt = m_pendingControllerRequests.find(prefix);
  if (pIt == m_pendingControllerRequests.end() || pIt->second.seq != seq)
  {
    return; // superseded or already completed
  }
  auto sIt = m_seqMap.find(prefix);
  if (sIt == m_seqMap.end() || sIt->second != seq)
  {
    // Release the pending record superseded by a newer prefix update.
    m_pendingControllerRequests.erase(pIt);
    return;
  }

  if (pIt->second.retriesLeft > 0)
  {
    --pIt->second.retriesLeft;
    ++m_counters.controllerRetries;
    ++m_counters.controlPackets; // repeated data-plane-initiated control request
    const ControlMessage retry = pIt->second.msg;
    if (m_flowTableSyncRequestCb)
    {
      m_flowTableSyncRequestCb(retry);
    }
    // An idempotent completion can be delivered synchronously by the callback.
    // Re-find the pending transaction before scheduling its next retry.
    pIt = m_pendingControllerRequests.find(prefix);
    if (pIt != m_pendingControllerRequests.end() && pIt->second.seq == seq)
    {
      pIt->second.timer = Simulator::Schedule(m_controllerRetryTimeout, &LocalRouteController::onControllerRetryTimeout,
                                              this, prefix, seq);
    }
    return;
  }

  // Exhausted retries withdraw the temporary entry and its pending metadata.
  // A flow-table entry installed by a successful FlowMod retains its own
  // expiry deadline, including when its completion notice was lost.
  ++m_counters.temporaryRouteWithdrawals;
  std::cerr << "[TEMPORARY_ROUTE_WITHDRAWAL] t=" << Simulator::Now().GetSeconds() << " prefix=" << prefix
            << " seq=" << seq << " retry_limit=" << m_controllerRetryLimit << std::endl;
  m_temporaryFib.erase(prefix);
  m_seqMap.erase(prefix);
  m_routeExpiries.erase(prefix);
  auto ltIt = m_lifetimeTimers.find(prefix);
  if (ltIt != m_lifetimeTimers.end())
  {
    Simulator::Cancel(ltIt->second);
    m_lifetimeTimers.erase(ltIt);
  }
  m_pendingControllerRequests.erase(pIt);
}

} // namespace statim
} // namespace ndn
} // namespace ns3
