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

#ifndef NDN_L3_PROTOCOL_H
#define NDN_L3_PROTOCOL_H

#include "ns3/ndnSIM/model/ndn-common.hpp"

#include <list>
#include <vector>

#include "ns3/ptr.h"
#include "ns3/net-device.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"

#include <boost/property_tree/ptree_fwd.hpp>

namespace nfd {
class Forwarder;
class FibManager;
class StrategyChoiceManager;
typedef boost::property_tree::ptree ConfigSection;
namespace pit {
class Entry;
} // namespace pit
namespace cs {
class Policy;
} // namespace cs
} // namespace nfd

namespace ns3 { namespace ndn { namespace statim {
class ForwardingEngine;
} } }

namespace ns3 {

class Packet;
class Node;
class Header;

namespace ndn {

class L3Protocol : boost::noncopyable, public Object {
public:

  static TypeId
  GetTypeId();

  static const uint16_t ETHERNET_FRAME_TYPE; ///< @brief Ethernet Frame Type of Ndn
  static const uint16_t IP_STACK_PORT;       ///< @brief TCP/UDP port for NDN stack
  // static const uint16_t IP_PROTOCOL_TYPE;    ///< \brief IP protocol type of NDN


  L3Protocol();

  virtual ~L3Protocol();


  shared_ptr<nfd::Forwarder>
  getForwarder();


  shared_ptr<nfd::FibManager>
  getFibManager();


  shared_ptr<nfd::StrategyChoiceManager>
  getStrategyChoiceManager();

  bool
  getEnableStatimPacket() const { return m_enableStatimPacket; }


  nfd::FaceId
  addFace(shared_ptr<Face> face);


  shared_ptr<Face>
  getFaceById(nfd::FaceId face) const;

  //
  // virtual void
  // removeFace(shared_ptr<Face> face);


  shared_ptr<Face>
  getFaceByNetDevice(Ptr<NetDevice> netDevice) const;


  nfd::ConfigSection&
  getConfig();


  void
  injectInterest(const Interest& interest);

  typedef std::function<std::unique_ptr<nfd::cs::Policy>()> PolicyCreationCallback;


  void
  setCsReplacementPolicy(const PolicyCreationCallback& policy);

public: // Workaround for python bindings
  static Ptr<L3Protocol>
  getL3Protocol(Ptr<Object> node);

public:
  typedef void (*InterestTraceCallback)(const Interest&, const Face&);
  typedef void (*DataTraceCallback)(const Data&, const Face&);

  typedef void (*SatisfiedInterestsCallback)(const nfd::pit::Entry& pitEntry, const Face& inFace, const Data& data);
  typedef void (*TimedOutInterestsCallback)(const nfd::pit::Entry& pitEntry);
  typedef void (*NackTraceCallback)(const lp::Nack&, const Face&);

protected:
  virtual void
  DoDispose(void); ///< @brief Do cleanup


  virtual void
  NotifyNewAggregate();

private:
  void
  initialize();

  void
  initializeManagement();

  void
  initializeRibManager();

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;

  // These objects are aggregated, but for optimization, get them here
  Ptr<Node> m_node; ///< \brief node on which ndn stack is installed

  TracedCallback<const Interest&, const Face&>
    m_inInterests; ///< @brief trace of incoming Interests
  TracedCallback<const Interest&, const Face&>
    m_outInterests; ///< @brief Transmitted interests trace

  TracedCallback<const Data&, const Face&> m_outData; ///< @brief trace of outgoing Data
  TracedCallback<const Data&, const Face&> m_inData;  ///< @brief trace of incoming Data

  TracedCallback<const lp::Nack&, const Face&> m_outNack; ///< @brief trace of outgoing Nack
  TracedCallback<const lp::Nack&, const Face&> m_inNack;  ///< @brief trace of incoming Nack

  TracedCallback<const nfd::pit::Entry&, const Face&, const Data&> m_satisfiedInterests;
  TracedCallback<const nfd::pit::Entry&> m_timedOutInterests;

  bool m_doPull;        // re-express pending interest on new trace (new nexthop for a prefix)
  bool m_allowTempPath; // allow forwarding according to trace Interest in-record, if enabled along
                        // with pulling, allow TI to pull pending Interests

  bool m_setTraceLifetime; // set lifetime of the nexthop according to trace lifetime field in TI
  bool m_prolongTrace;     // keep trace alive on dataflow

  bool m_removeTraceOnNack;
  bool m_enableStatimPacket;
  bool m_useStatimEngine;

public:

  std::shared_ptr<statim::ForwardingEngine>
  getStatimEngine();


  bool
  isUsingStatimEngine() const { return m_useStatimEngine; }


  std::vector<shared_ptr<Face>>
  getRegisteredFaces() const;
};

} // namespace ndn
} // namespace ns3

#endif
