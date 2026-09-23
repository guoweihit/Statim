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

#include "ns3/ndnSIM/statim/helper/statim-helper.hpp"

#include "ns3/boolean.h"
#include "ns3/log.h"
#include "ns3/node-list.h"

#include "ns3/ndnSIM/statim/forwarding-engine.hpp"
#include "ns3/ndnSIM/statim/packet-header.hpp"
#include "ns3/ndnSIM/statim/port-table.hpp"

#include "ns3/ndnSIM/model/ndn-global-router.hpp"
#include "ns3/ndnSIM/model/ndn-l3-protocol.hpp"
#include "ns3/ndnSIM/model/ndn-net-device-transport.hpp"

#include "ns3/ndnSIM/helper/boost-graph-ndn-global-routing-helper.hpp"
#include <boost/graph/dijkstra_shortest_paths.hpp>

NS_LOG_COMPONENT_DEFINE("ndn.StatimHelper");

namespace ns3
{
namespace ndn
{

StatimHelper::StatimHelper()
    : m_controllerDelay(Seconds(2.0)), m_flowTableFibEntryLifetime(Seconds(0)),
      m_temporaryFibEntryLifetime(Seconds(0)), m_statefulDelay(Seconds(0)), m_controllerRetryTimeout(Seconds(0)),
      m_enableInterestReforwarding(true), m_enableTemporaryFib(true), m_enableStatimPacket(true),
      m_experimentTelemetryEnabled(false), m_generationGuardEnabled(false), m_interestReforwardingLimit(30),
      m_controllerRetryLimit(3), m_syncAckLossRate(0.0)
{
  m_stackHelper.SetStackAttributes("EnableStatimPacket", "true", "DoPull", "true",
                                   "UseStatimEngine", "true");
}

StatimHelper::~StatimHelper() {}

void StatimHelper::SetDefaultRoutes(bool needSet)
{
  m_stackHelper.SetDefaultRoutes(needSet);
}

void StatimHelper::SetControllerDelay(Time delay)
{
  m_controllerDelay = delay;
}

void StatimHelper::SetEnableInterestReforwarding(bool enableInterestReforwarding)
{
  m_enableInterestReforwarding = enableInterestReforwarding;
  m_stackHelper.SetStackAttributes("DoPull", enableInterestReforwarding ? "true" : "false");
}

void StatimHelper::SetEnableTemporaryFib(bool enable)
{
  m_enableTemporaryFib = enable;
}

void StatimHelper::SetEnableStatimPacket(bool enable)
{
  m_enableStatimPacket = enable;
  m_stackHelper.SetStackAttributes("EnableStatimPacket", enable ? "true" : "false");
}

void StatimHelper::SetOldContentStore(const std::string& contentStoreClass,
                                      const std::string& attr1, const std::string& value1,
                                      const std::string& attr2, const std::string& value2,
                                      const std::string& attr3, const std::string& value3,
                                      const std::string& attr4, const std::string& value4)
{
  m_stackHelper.SetOldContentStore(contentStoreClass, attr1, value1, attr2, value2, attr3, value3,
                                   attr4, value4);
}

void StatimHelper::setCsSize(size_t maxSize)
{
  m_stackHelper.setCsSize(maxSize);
}

void StatimHelper::setPolicy(const std::string& policy)
{
  m_stackHelper.setPolicy(policy);
}

Ptr<FaceContainer> StatimHelper::Install(Ptr<Node> node) const
{
  bool enableStatimPacket = ShouldEnableStatimPacketOnNode(node);
  m_stackHelper.SetStackAttributes("EnableStatimPacket", enableStatimPacket ? "true" : "false");

  Ptr<FaceContainer> faces = m_stackHelper.Install(node);

  ConfigureInstalledNode(node, m_controllerDelay, m_enableInterestReforwarding, m_enableTemporaryFib, m_statefulDelay,
                         m_interestReforwardingLimit, m_controllerRetryTimeout, m_controllerRetryLimit, m_generationGuardEnabled,
                         m_syncAckLossRate, m_experimentTelemetryEnabled);

  if (m_flowTableFibEntryLifetime > Seconds(0) || m_temporaryFibEntryLifetime > Seconds(0))
  {
    ConfigureEntryLifetimes(node, m_flowTableFibEntryLifetime, m_temporaryFibEntryLifetime);
  }
  return faces;
}

Ptr<FaceContainer> StatimHelper::Install(const NodeContainer& c) const
{
  Ptr<FaceContainer> faces = Create<FaceContainer>();
  for (NodeContainer::Iterator i = c.Begin(); i != c.End(); ++i)
  {
    faces->AddAll(Install(*i));
  }
  return faces;
}

Ptr<FaceContainer> StatimHelper::InstallAll() const
{
  return Install(NodeContainer::GetGlobal());
}

void StatimHelper::ConfigureInstalledNode(Ptr<Node> node, Time controllerDelay, bool enableInterestReforwarding,
                                          bool enableTemporaryFib, Time statefulDelay, int interestReforwardingLimit,
                                          Time controllerRetryTimeout, int controllerRetryLimit,
                                          bool generationGuardEnabled, double syncAckLossRate,
                                          bool experimentTelemetryEnabled)
{
  Ptr<L3Protocol> l3 = node->GetObject<L3Protocol>();
  if (l3 == nullptr)
    return;
  if (l3->isUsingStatimEngine())
  {
    auto engine = l3->getStatimEngine();
    if (engine == nullptr)
    {
      NS_LOG_WARN("Node " << node->GetId() << " has no ForwardingEngine");
      return;
    }
    l3->SetAttribute("DoPull", BooleanValue(enableInterestReforwarding));
    engine->setExperimentTelemetryEnabled(experimentTelemetryEnabled);
    if (experimentTelemetryEnabled)
    {
      engine->setNodeId(node->GetId());
    }
    engine->setControllerDelay(controllerDelay);
    engine->setEnableInterestReforwarding(enableInterestReforwarding);
    engine->setEnableTemporaryFib(enableTemporaryFib);
    engine->setStatefulDelay(statefulDelay);
    engine->setInterestReforwardingLimit(interestReforwardingLimit);
    engine->setControllerRetryTimeout(controllerRetryTimeout);
    engine->setControllerRetryLimit(controllerRetryLimit);
    engine->setGenerationGuardEnabled(generationGuardEnabled);
    engine->setSyncAckLossRate(syncAckLossRate);
    return;
  }
}

void StatimHelper::ConfigureEntryLifetimes(Ptr<Node> node, Time flowTableFibEntryLifetime,
                                           Time temporaryFibEntryLifetime)
{
  Ptr<L3Protocol> l3 = node->GetObject<L3Protocol>();
  if (l3 == nullptr)
    return;

  if (l3->isUsingStatimEngine())
  {
    auto engine = l3->getStatimEngine();
    if (engine == nullptr)
      return;
    if (flowTableFibEntryLifetime > Seconds(0))
    {
      engine->setFlowTableFibEntryLifetime(flowTableFibEntryLifetime);
    }
    if (temporaryFibEntryLifetime > Seconds(0))
    {
      engine->setTemporaryFibEntryLifetime(temporaryFibEntryLifetime);
    }
    return;
  }
}

bool StatimHelper::ShouldEnableStatimPacketOnNode(Ptr<Node> node) const
{
  return m_enableStatimPacket && node != nullptr;
}

void StatimHelper::CalculateStatimRoutes()
{
  BOOST_CONCEPT_ASSERT((boost::VertexListGraphConcept<boost::NdnGlobalRouterGraph>));
  BOOST_CONCEPT_ASSERT((boost::IncidenceGraphConcept<boost::NdnGlobalRouterGraph>));

  boost::NdnGlobalRouterGraph graph;

  for (NodeList::Iterator node = NodeList::Begin(); node != NodeList::End(); node++)
  {

    Ptr<GlobalRouter> source = (*node)->GetObject<GlobalRouter>();
    if (source == nullptr)
    {
      NS_LOG_DEBUG("Node " << (*node)->GetId() << " has no GlobalRouter interface");
      continue;
    }

    Ptr<L3Protocol> l3 = (*node)->GetObject<L3Protocol>();
    if (!l3 || !l3->isUsingStatimEngine())
    {
      continue;
    }

    auto engine = l3->getStatimEngine();
    if (!engine)
      continue;

    boost::DistancesMap distances;
    dijkstra_shortest_paths(graph, source,
                            distance_map(boost::ref(distances))
                                .distance_inf(boost::WeightInf)
                                .distance_zero(boost::WeightZero)
                                .distance_compare(boost::WeightCompare())
                                .distance_combine(boost::WeightCombine()));

    auto& statimSwitch = engine->getSwitch();
    auto& portTable = engine->getPortTable();

    for (const auto& dist : distances)
    {
      if (dist.first == source)
        continue;
      if (std::get<0>(dist.second) == nullptr)
        continue;

      shared_ptr<Face> nextHopFace = std::get<0>(dist.second);

      auto* ndTransport = dynamic_cast<NetDeviceTransport*>(nextHopFace->getTransport());
      if (ndTransport == nullptr)
        continue;

      uint16_t port = portTable.getPort(ndTransport->GetNetDevice());

      for (const auto& prefix : dist.first->GetLocalPrefixes())
      {
        statim::PacketHeader hdr(*prefix);
        auto hashes = hdr.getNameHashes();
        statimSwitch.installFibEntry(hashes, port);
      }
    }
  }
}

} // namespace ndn
} // namespace ns3
