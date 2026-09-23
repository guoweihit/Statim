/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2017 Harbin Institute of Technology, China
 *
 * Author: Zhongda Xia <xiazhongda@hit.edu.cn>
 **/

#include "trace-forwarding.hpp"

#include "core/logger.hpp"

NFD_LOG_INIT("TraceForwardingStrategy");

namespace nfd {
namespace fw {

const Name TraceForwardingStrategy::STRATEGY_NAME("ndn:/localhost/nfd/strategy/trace-forwarding");

TraceForwardingStrategy::TraceForwardingStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder, name)
{
}

TraceForwardingStrategy::~TraceForwardingStrategy()
{
}

void
TraceForwardingStrategy::beforeExpirePendingInterest(const shared_ptr<pit::Entry>& pitEntry)
{
}

static bool
canForwardToNextHop(const Face& inFace, shared_ptr<pit::Entry> pitEntry,
                    const fib::NextHop& nexthop)
{
  return !wouldViolateScope(inFace, pitEntry->getInterest(), nexthop.getFace())
         && canForwardToLegacy(*pitEntry, nexthop.getFace());
}

static bool
hasFaceForForwarding(const Face& inFace, const fib::NextHopList& nexthops,
                     const shared_ptr<pit::Entry>& pitEntry)
{
  return std::find_if(nexthops.begin(), nexthops.end(),
                      bind(&canForwardToNextHop, cref(inFace), pitEntry, _1)) != nexthops.end();
}

void
TraceForwardingStrategy::afterReceiveInterest(const Face& inFace, const Interest& interest,
                                              const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_TRACE("afterReceiveInterest");

  if (hasPendingOutRecords(*pitEntry)) {
    // not a new Interest, don't forward
    return;
  }

  const tib::Entry& tibEntry = this->lookupTib(*pitEntry);
  if (tibEntry.hasNextHops()) {
    if (TraceForwarding(inFace, interest, pitEntry, tibEntry)) {
      return;
    }
  }

  const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  const fib::NextHopList& nexthops = fibEntry.getNextHops();

  // Ensure there is at least 1 Face available for forwarding
  if (!hasFaceForForwarding(inFace, nexthops, pitEntry)) {
    this->rejectPendingInterest(pitEntry);
    return;
  }

  // flood it
  for (fib::NextHopList::const_iterator it = nexthops.begin(); it != nexthops.end(); ++it) {
    if (canForwardToNextHop(inFace, pitEntry, *it)) {
      NFD_LOG_INFO("FIB forwarding to face: " << it->getFace().getId()
                                              << ", prefix: " << fibEntry.getPrefix()
                                              << ", name: " << interest.getName());
      this->sendInterest(pitEntry, it->getFace(), interest);
    }
  }
}

void
TraceForwardingStrategy::afterReceiveNack(const Face& inFace, const lp::Nack& nack,
                                          const shared_ptr<pit::Entry>& pitEntry)
{
  Strategy::afterReceiveNack(inFace, nack, pitEntry);

  this->sendNacks(pitEntry, nack.getHeader());
}

bool
TraceForwardingStrategy::TraceForwarding(const Face& inFace, const Interest& interest,
                                         const shared_ptr<pit::Entry>& pitEntry,
                                         const tib::Entry& tibEntry)
{
  const tib::NextHopList& nexthops = tibEntry.getNextHops();

  bool flagSent = false;

  // flood it
  for (tib::NextHopList::const_iterator it = nexthops.begin(); it != nexthops.end(); ++it) {
    if (canForwardToNextHop(inFace, pitEntry, *it)) {
      flagSent = true;
      NFD_LOG_INFO("TIB forwarding to face: " << it->getFace().getId()
                                              << ", prefix: " << tibEntry.getPrefix()
                                              << ", name: " << interest.getName());
      this->sendInterest(pitEntry, it->getFace(), interest);
    }
    else {
      NFD_LOG_INFO("TIB can't forward to face: " << it->getFace().getId()
                                              << ", prefix: " << tibEntry.getPrefix()
                                              << ", name: " << interest.getName());
    }
  }

  return flagSent;
}

} // namespace fw
} // namespace nfd
