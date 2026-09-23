// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

//  Wired handover: simulate mobile handover via P2P link switching
// -----------------------------------------------------------------------------
//  Creates P2P links between the mobile node and all backbone nodes, then
//  periodically checks distance and switches the "active" link by toggling
//  error models (fail/up) on the P2P NetDevices. Association callbacks let the
//  scenario update the mobile's FIB for the selected backbone node.
//
//  Keep the current AP while it is within RANGE_M (100 m); otherwise select
//  the nearest in-range node, or detach if no node is in range.
//
//  Usage (in a scenario main()):
//      #include "ns3/ndnSIM/statim/scenarios/common/wired-handover.hpp"
//      ...
//      // After mobility is installed on backbone + mobile, BEFORE ndnHelper:
//      wiredhandover::CreateLinks(mobileNode, backboneNodes, "54Mbps", "2ms");
//      // After ndnHelper.Install on all nodes:
//      wiredhandover::Setup(interval, assocCallback, deassocCallback);
//      // Callbacks receive (mobile, newApIndex, oldApIndex) and (mobile, oldApIndex).

#ifndef WIRED_HANDOVER_H
#define WIRED_HANDOVER_H

#include "ns3/boolean.h"
#include "ns3/core-module.h"
#include "ns3/double.h"
#include "ns3/error-model.h"
#include "ns3/mobility-module.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/point-to-point-module.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/pointer.h"
#include "ns3/string.h"

#include <functional>
#include <vector>

namespace ns3
{
namespace wiredhandover
{

static const double RANGE_M = 100.0;

inline std::vector<Ptr<Node>>& BackboneNodes()
{
  static std::vector<Ptr<Node>> v;
  return v;
}
inline Ptr<Node>& MobileNode()
{
  static Ptr<Node> n;
  return n;
}
inline int& ActiveAp()
{
  static int a = -1;
  return a;
}
inline std::function<void(Ptr<Node>, int, int)>& AssocCb()
{
  static std::function<void(Ptr<Node>, int, int)> cb;
  return cb;
}
inline std::function<void(Ptr<Node>, int)>& DeassocCb()
{
  static std::function<void(Ptr<Node>, int)> cb;
  return cb;
}

inline void FailP2PLink(Ptr<Node> n1, Ptr<Node> n2)
{
  for (uint32_t d = 0; d < n1->GetNDevices(); ++d)
  {
    Ptr<PointToPointNetDevice> ppd = n1->GetDevice(d)->GetObject<PointToPointNetDevice>();
    if (!ppd)
      continue;
    Ptr<PointToPointChannel> ch = DynamicCast<PointToPointChannel>(ppd->GetChannel());
    if (!ch)
      continue;
    Ptr<NetDevice> remote = ch->GetDevice(0);
    if (remote->GetNode() == n1)
      remote = ch->GetDevice(1);
    if (remote->GetNode() == n2)
    {
      ObjectFactory ef("ns3::RateErrorModel");
      ef.Set("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));
      ef.Set("ErrorRate", DoubleValue(1.0));
      ppd->SetAttribute("ReceiveErrorModel", PointerValue(ef.Create<ErrorModel>()));
      remote->GetObject<PointToPointNetDevice>()->SetAttribute(
          "ReceiveErrorModel", PointerValue(ef.Create<ErrorModel>()));
      return;
    }
  }
}

inline void UpP2PLink(Ptr<Node> n1, Ptr<Node> n2)
{
  for (uint32_t d = 0; d < n1->GetNDevices(); ++d)
  {
    Ptr<PointToPointNetDevice> ppd = n1->GetDevice(d)->GetObject<PointToPointNetDevice>();
    if (!ppd)
      continue;
    Ptr<PointToPointChannel> ch = DynamicCast<PointToPointChannel>(ppd->GetChannel());
    if (!ch)
      continue;
    Ptr<NetDevice> remote = ch->GetDevice(0);
    if (remote->GetNode() == n1)
      remote = ch->GetDevice(1);
    if (remote->GetNode() == n2)
    {
      Ptr<PointToPointNetDevice> rppd = remote->GetObject<PointToPointNetDevice>();
      ObjectFactory ef("ns3::RateErrorModel");
      ef.Set("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));
      ef.Set("ErrorRate", DoubleValue(0.0));
      ef.Set("IsEnabled", BooleanValue(false));
      ppd->SetAttribute("ReceiveErrorModel", PointerValue(ef.Create<ErrorModel>()));
      rppd->SetAttribute("ReceiveErrorModel", PointerValue(ef.Create<ErrorModel>()));
      return;
    }
  }
}

inline int Pick(const Vector& p)
{
  if (ActiveAp() >= 0)
  {
    Vector apPos = BackboneNodes()[ActiveAp()]->GetObject<MobilityModel>()->GetPosition();
    if (CalculateDistance(p, apPos) <= RANGE_M)
      return ActiveAp();
  }
  int best = -1;
  double bestd = 1e18;
  for (size_t i = 0; i < BackboneNodes().size(); ++i)
  {
    double d = CalculateDistance(p, BackboneNodes()[i]->GetObject<MobilityModel>()->GetPosition());
    if (d <= RANGE_M && d < bestd)
    {
      bestd = d;
      best = (int)i;
    }
  }
  return best;
}

inline void Step(double interval)
{
  Ptr<MobilityModel> mob = MobileNode()->GetObject<MobilityModel>();
  int best = Pick(mob->GetPosition());

  if (best != ActiveAp())
  {
    int oldAp = ActiveAp();
    if (oldAp >= 0)
    {
      std::cerr << "[WIRE-HO] t=" << Simulator::Now().GetSeconds() << " FAIL link mobile<->node"
                << oldAp << "\n";
      FailP2PLink(MobileNode(), BackboneNodes()[oldAp]);
      if (DeassocCb())
        DeassocCb()(MobileNode(), oldAp);
    }
    if (best >= 0)
    {
      std::cerr << "[WIRE-HO] t=" << Simulator::Now().GetSeconds() << " UP link mobile<->node"
                << best;
      // verify the link is found
      bool found = false;
      for (uint32_t d = 0; d < MobileNode()->GetNDevices(); ++d)
      {
        Ptr<PointToPointNetDevice> ppd =
            MobileNode()->GetDevice(d)->GetObject<PointToPointNetDevice>();
        if (!ppd)
          continue;
        Ptr<PointToPointChannel> ch = DynamicCast<PointToPointChannel>(ppd->GetChannel());
        if (!ch)
          continue;
        Ptr<NetDevice> remote = ch->GetDevice(0);
        if (remote->GetNode() == MobileNode())
          remote = ch->GetDevice(1);
        if (remote->GetNode() == BackboneNodes()[best])
        {
          found = true;
          break;
        }
      }
      std::cerr << " linkFound=" << found << "\n";
      UpP2PLink(MobileNode(), BackboneNodes()[best]);
      if (AssocCb())
        AssocCb()(MobileNode(), best, oldAp);
    }
    ActiveAp() = best;
  }
  Simulator::Schedule(Seconds(interval), &Step, interval);
}

// Call BEFORE ndnHelper.Install() — creates the P2P links
inline void CreateLinks(Ptr<Node> mobileNode, const std::vector<Ptr<Node>>& backboneNodes,
                        const std::string& dataRate = "54Mbps", const std::string& delay = "2ms")
{
  MobileNode() = mobileNode;
  BackboneNodes() = backboneNodes;

  PointToPointHelper p2pMobile;
  p2pMobile.SetDeviceAttribute("DataRate", StringValue(dataRate));
  p2pMobile.SetChannelAttribute("Delay", StringValue(delay));

  for (size_t i = 0; i < backboneNodes.size(); ++i)
  {
    p2pMobile.Install(mobileNode, backboneNodes[i]);
  }
}

// Call AFTER ndnHelper.Install() and apps installed — starts the handover loop
inline void Setup(double interval, std::function<void(Ptr<Node>, int, int)> assocCb,
                  std::function<void(Ptr<Node>, int)> deassocCb = nullptr)
{
  AssocCb() = assocCb;
  DeassocCb() = deassocCb;

  // Fail all links initially
  for (size_t i = 0; i < BackboneNodes().size(); ++i)
  {
    FailP2PLink(MobileNode(), BackboneNodes()[i]);
  }
  ActiveAp() = -1;

  // Start the periodic check 0.2 s after Setup is called.
  Simulator::Schedule(Seconds(0.2), &Step, interval);
}

} // namespace wiredhandover
} // namespace ns3

#endif // WIRED_HANDOVER_H
