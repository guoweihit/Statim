// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/ndnSIM-module.h"
#include "ns3/point-to-point-module.h"

#include "ns3/ndnSIM/apps/kite-pull-consumer.hpp"
#include "ns3/ndnSIM/apps/kite-pull-mobile.hpp"

#include "ns3/ndnSIM/statim/scenarios/common/wired-handover.hpp"

#include "ns3/ndnSIM/NFD/daemon/fw/trace-forwarding.hpp"
#include "ns3/ndnSIM/model/ndn-global-router.hpp"
#include "ns3/ndnSIM/model/ndn-l3-protocol.hpp"
#include "ns3/ndnSIM/model/ndn-net-device-transport.hpp"
#include "ns3/ndnSIM/utils/tracers/ndn-app-delay-tracer.hpp"

#include "ns3/ndnSIM/NFD/daemon/table/fib-entry.hpp"
#include "ns3/ndnSIM/NFD/daemon/table/fib.hpp"

#include <algorithm>
#include <iomanip>

NS_LOG_COMPONENT_DEFINE("kite.grid");

namespace ns3
{

// ---------------------------------------------------------------- globals
static Ptr<Node> g_mobileNode = nullptr;

// -------------------------------------------------- direct FIB management
static nfd::FaceId FindFaceIdToNode(Ptr<Node> fromNode, Ptr<Node> toNode)
{
  Ptr<ndn::L3Protocol> l3 = fromNode->GetObject<ndn::L3Protocol>();
  if (!l3)
    return nfd::face::INVALID_FACEID;
  auto forwarder = l3->getForwarder();
  if (!forwarder)
    return nfd::face::INVALID_FACEID;
  for (const auto& face : forwarder->getFaceTable())
  {
    auto transport = dynamic_cast<ndn::NetDeviceTransport*>(face.getTransport());
    if (!transport)
      continue;
    Ptr<PointToPointNetDevice> ppd = DynamicCast<PointToPointNetDevice>(transport->GetNetDevice());
    if (!ppd)
      continue;
    Ptr<Channel> ch = ppd->GetChannel();
    if (!ch)
      continue;
    Ptr<NetDevice> remote = ch->GetDevice(0);
    if (remote->GetNode() == fromNode)
      remote = ch->GetDevice(1);
    if (remote->GetNode() == toNode)
      return face.getId();
  }
  return nfd::face::INVALID_FACEID;
}

static void DirectFibSet(Ptr<Node> node, const ndn::Name& prefix, nfd::FaceId faceId)
{
  auto forwarder = node->GetObject<ndn::L3Protocol>()->getForwarder();
  nfd::Face* face = forwarder->getFaceTable().get(faceId);
  if (!face)
    return;
  auto& fib = forwarder->getFib();
  auto result = fib.insert(prefix);
  result.first->addNextHop(*face, 0);
}

static void DirectFibClear(Ptr<Node> node, const ndn::Name& prefix)
{
  auto l3 = node->GetObject<ndn::L3Protocol>();
  if (!l3)
    return;
  auto forwarder = l3->getForwarder();
  if (!forwarder)
    return;
  forwarder->getFib().erase(prefix);
}

// ------------------------------------------------------------- handover
static void InitialAttach(Ptr<Node> apOld, Ptr<Node> apNew)
{
  wiredhandover::FailP2PLink(g_mobileNode, apNew);
  nfd::FaceId fid = FindFaceIdToNode(g_mobileNode, apOld);
  if (fid != nfd::face::INVALID_FACEID)
    DirectFibSet(g_mobileNode, ndn::Name("/"), fid);

  apOld->GetObject<ndn::L3Protocol>()->getForwarder()->m_gone = false;

  ndn::KitePullMobile* mobileApp =
      dynamic_cast<ndn::KitePullMobile*>(&(*g_mobileNode->GetApplication(0)));
  if (mobileApp)
  {
    mobileApp->m_current = apOld->GetId();
    mobileApp->OnAssociation();
  }
  std::cerr << "[ATTACH t=" << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds()
            << "s] initial AP=R" << (apOld->GetId() + 1) << std::endl;
}

static void DoHandover(Ptr<Node> apOld, Ptr<Node> apNew)
{
  // Detach and reattach at the same simulation instant.
  wiredhandover::FailP2PLink(g_mobileNode, apOld);
  wiredhandover::UpP2PLink(g_mobileNode, apNew);

  DirectFibClear(g_mobileNode, ndn::Name("/"));
  nfd::FaceId fid = FindFaceIdToNode(g_mobileNode, apNew);
  if (fid != nfd::face::INVALID_FACEID)
    DirectFibSet(g_mobileNode, ndn::Name("/"), fid);

  apNew->GetObject<ndn::L3Protocol>()->getForwarder()->m_gone = false;

  ndn::KitePullMobile* mobileApp =
      dynamic_cast<ndn::KitePullMobile*>(&(*g_mobileNode->GetApplication(0)));
  if (mobileApp)
  {
    mobileApp->m_current = apNew->GetId();
    mobileApp->OnAssociation();                // sends the handover TI
    mobileApp->MarkHandover(Simulator::Now()); // T_ho anchor = this instant
  }
  std::cerr << "[HANDOVER t=" << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds()
            << "s] R" << (apOld->GetId() + 1) << " -> R" << (apNew->GetId() + 1)
            << " (detach and re-attach at the same instant)" << std::endl;
}

int main(int argc, char* argv[])
{
  Config::SetDefault("ns3::PointToPointNetDevice::DataRate", StringValue("1Gbps"));
  Config::SetDefault("ns3::PointToPointChannel::Delay", StringValue("10ms"));
  Config::SetDefault("ns3::DropTailQueue::MaxPackets", StringValue("10000"));

  uint32_t run = 1;
  double stopTime = 32.0;
  bool enableInterestReforwarding = true; // KITE original pull (part of the baseline)
  bool consumerRetransmissions = true;
  double consumerCbrFreq = 1000.0;
  double interestLifetimeSeconds = 2;
  double initialRttEstimate = 2;
  double traceLifeTime = 2;
  double refreshInterval = 2;
  int payloadSize = 1024;
  double hoBase = 20.0;
  double hoJitter = 1.0;
  double windowLen = 4.0;
  std::string appDelayFile = "";

  CommandLine cmd;
  cmd.AddValue("run", "ns-3 random-stream run number (positive integer)", run);
  cmd.AddValue("stop", "stop time (s)", stopTime);
  cmd.AddValue("enableInterestReforwarding",
               "reforward pending Interests through the KITE route-update handler",
               enableInterestReforwarding);
  cmd.AddValue("consumerRetransmissions",
               "enable consumer Interest retransmissions during the simulation",
               consumerRetransmissions);
  cmd.AddValue("consumerCbrFreq", "consumer Interest rate r (1/s)", consumerCbrFreq);
  cmd.AddValue("interestLifetime", "Interest packet lifetime (seconds > 0)", interestLifetimeSeconds);
  cmd.AddValue("initialRttEstimate", "Initial RTT estimate before RTT samples (seconds > 0)", initialRttEstimate);
  cmd.AddValue("traceLifeTime", "trace lifetime (s)", traceLifeTime);
  cmd.AddValue("refreshInterval",
               "periodic Trace Interest refresh interval (seconds)",
               refreshInterval);
  cmd.AddValue("payloadSize", "Data payload (bytes)", payloadSize);
  cmd.AddValue("hoBase", "handover base instant (s)", hoBase);
  cmd.AddValue("hoJitter", "handover jitter width (s)", hoJitter);
  cmd.AddValue("windowLen", "loss window length (s)", windowLen);
  cmd.AddValue("appDelayFile", "AppDelayTracer output file (empty=off)", appDelayFile);
  cmd.Parse(argc, argv);
  if (interestLifetimeSeconds <= 0 || initialRttEstimate <= 0)
  {
    NS_FATAL_ERROR("Interest lifetime and initial RTT estimate must be positive");
  }

  Config::SetGlobal("RngRun", IntegerValue(run));
  Config::SetDefault("ns3::ndn::RttEstimator::InitialEstimation", TimeValue(Seconds(initialRttEstimate)));

  Config::SetDefault("ns3::ndn::L3Protocol::DoPull", BooleanValue(enableInterestReforwarding));

  // handover instant derived from the run seed (same stream as statim-grid)
  Ptr<UniformRandomVariable> hoRand = CreateObject<UniformRandomVariable>();
  hoRand->SetStream(9000);
  double tHo = hoBase + hoRand->GetValue(0.0, 1.0) * hoJitter;

  std::cerr << std::defaultfloat << std::setprecision(17)
            << "[CONFIG] scenario=kite-grid run=" << run << " enableInterestReforwarding=" << (enableInterestReforwarding ? 1 : 0)
            << " interestLifetime=" << interestLifetimeSeconds << " initialRttEstimate=" << initialRttEstimate
            << " consumerRetransmissions=" << (consumerRetransmissions ? 1 : 0) << " r=" << consumerCbrFreq
            << " t_ho=" << tHo << " windowLen=" << windowLen
            << " stop=" << stopTime << std::endl;

  // ---------------- topology: R1..R6 grid + consumer + RV ----------------
  NodeContainer nodes;
  AnnotatedTopologyReader topologyReader("", 25);
  topologyReader.SetFileName("src/ndnSIM/statim/scenarios/common/grid-topology.txt");
  nodes = topologyReader.Read(); // R1=0 R2=1 R3=2 R4=3 R5=4 R6=5
  nodes.Create(2);               // consumer=6, RV=7

  PointToPointHelper p2p;
  p2p.Install(nodes.Get(6), nodes.Get(3)); // consumer -- R4
  p2p.Install(nodes.Get(7), nodes.Get(0)); // RV -- R1

  MobilityHelper mobility;
  Ptr<ListPositionAllocator> posAlloc = CreateObject<ListPositionAllocator>();
  posAlloc->Add(Vector(0, 200, 0));    // R1
  posAlloc->Add(Vector(100, 200, 0));  // R2
  posAlloc->Add(Vector(200, 200, 0));  // R3
  posAlloc->Add(Vector(0, 100, 0));    // R4
  posAlloc->Add(Vector(100, 100, 0));  // R5
  posAlloc->Add(Vector(200, 100, 0));  // R6
  posAlloc->Add(Vector(-100, 100, 0)); // consumer
  posAlloc->Add(Vector(-100, 200, 0)); // RV
  mobility.SetPositionAllocator(posAlloc);
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(nodes);

  // ---------------- mobile producer: wired links to R3 and R6 ------------
  NodeContainer mobileNodes;
  mobileNodes.Create(1);
  g_mobileNode = mobileNodes.Get(0);

  posAlloc = CreateObject<ListPositionAllocator>();
  posAlloc->Add(Vector(300, 150, 0));
  mobility.SetPositionAllocator(posAlloc);
  mobility.Install(mobileNodes);

  std::vector<Ptr<Node>> accessNodes;
  accessNodes.push_back(nodes.Get(2)); // R3
  accessNodes.push_back(nodes.Get(5)); // R6
  wiredhandover::CreateLinks(g_mobileNode, accessNodes, "1Gbps", "10ms");

  // ---------------- NFD stack everywhere --------------------------------
  ndn::StackHelper ndnHelper;
  ndnHelper.SetDefaultRoutes(true);
  ndnHelper.Install(nodes);
  ndnHelper.SetDefaultRoutes(false); // mobile FIB is managed via DirectFibSet
  ndnHelper.SetOldContentStore("ns3::ndn::cs::Nocache");
  ndnHelper.Install(mobileNodes);

  ndn::StrategyChoiceHelper::InstallAll<nfd::fw::TraceForwardingStrategy>("/");

  // Keep the mobile out of the static Dijkstra graph.
  g_mobileNode->AggregateObject(CreateObject<ndn::GlobalRouter>());

  std::string dataPrefix = "/alice/photo";
  std::string rvPrefix = "/rv";

  ndn::GlobalRoutingHelper ndnGlobalRoutingHelper;
  ndnGlobalRoutingHelper.Install(nodes);
  ndnGlobalRoutingHelper.AddOrigins(rvPrefix, nodes.Get(7));

  // ---------------- applications ----------------------------------------
  ndn::AppHelper consumerHelper("ns3::ndn::KitePullConsumer");
  consumerHelper.SetAttribute("Prefix", StringValue(rvPrefix + dataPrefix));
  consumerHelper.SetAttribute("Frequency", DoubleValue(consumerCbrFreq));
  consumerHelper.SetAttribute("LifeTime", TimeValue(Seconds(interestLifetimeSeconds)));
  consumerHelper.SetAttribute("LossWindowStart", TimeValue(Seconds(tHo)));
  consumerHelper.SetAttribute("LossWindowLen", TimeValue(Seconds(windowLen)));
  if (!consumerRetransmissions)
  {
    consumerHelper.SetAttribute("RetxTimer", TimeValue(Seconds(std::max(1000.0, stopTime + 1.0))));
  }
  ApplicationContainer consumerApp = consumerHelper.Install(nodes.Get(6));
  consumerApp.Start(Seconds(1.0));
  consumerApp.Stop(Seconds(stopTime - 2.0));

  ndn::AppHelper rvHelper("ns3::ndn::KiteRv");
  rvHelper.SetAttribute("RvPrefix", StringValue(rvPrefix));
  rvHelper.Install(nodes.Get(7));

  ndn::AppHelper mobileHelper("ns3::ndn::KitePullMobile");
  mobileHelper.SetPrefix(rvPrefix + dataPrefix);
  mobileHelper.SetAttribute("RvPrefix", StringValue(rvPrefix));
  mobileHelper.SetAttribute("DataPrefix", StringValue(dataPrefix));
  mobileHelper.SetAttribute("PayloadSize", StringValue(std::to_string(payloadSize)));
  mobileHelper.SetAttribute("TraceLifetime", TimeValue(Seconds(traceLifeTime)));
  mobileHelper.SetAttribute("RefreshInterval", TimeValue(Seconds(refreshInterval)));
  ApplicationContainer mobileApp = mobileHelper.Install(g_mobileNode);
  mobileApp.Stop(Seconds(stopTime - 1.0));

  ndn::GlobalRoutingHelper::CalculateRoutes();

  // ---------------- schedule: attach, handover ---------------------------
  Simulator::Schedule(Seconds(0.2), &InitialAttach, nodes.Get(2), nodes.Get(5));
  Simulator::Schedule(Seconds(tHo), &DoHandover, nodes.Get(2), nodes.Get(5));

  if (!appDelayFile.empty())
  {
    ndn::AppDelayTracer::InstallAll(appDelayFile);
  }

  Simulator::Stop(Seconds(stopTime));
  Simulator::Run();
  Simulator::Destroy();
  return 0;
}

} // namespace ns3

int main(int argc, char* argv[])
{
  return ns3::main(argc, argv);
}
