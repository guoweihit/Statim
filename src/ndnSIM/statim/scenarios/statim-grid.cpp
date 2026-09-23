// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/ndnSIM-module.h"
#include "ns3/point-to-point-module.h"

#include "ns3/ndnSIM/statim/apps/statim-consumer.hpp"
#include "ns3/ndnSIM/statim/apps/statim-mobile.hpp"
#include "ns3/ndnSIM/statim/helper/statim-helper.hpp"

#include "ns3/ndnSIM/statim/scenarios/common/wired-handover.hpp"

#include "ns3/ndnSIM/model/ndn-global-router.hpp"
#include "ns3/ndnSIM/model/ndn-l3-protocol.hpp"
#include "ns3/ndnSIM/model/ndn-net-device-transport.hpp"
#include "ns3/ndnSIM/statim/forwarding-engine.hpp"
#include "ns3/ndnSIM/statim/packet-header.hpp"
#include "ns3/ndnSIM/utils/tracers/ndn-app-delay-tracer.hpp"

#include "ns3/ndnSIM/NFD/daemon/table/fib-entry.hpp"
#include "ns3/ndnSIM/NFD/daemon/table/fib.hpp"

#include <algorithm>
#include <iomanip>

NS_LOG_COMPONENT_DEFINE("statim.grid");

namespace ns3
{

// ---------------------------------------------------------------- globals
static Ptr<Node> g_mobileNode = nullptr;
static NodeContainer g_backboneNodes;
static int g_oldApId = -1;

static std::map<uint32_t, size_t> g_temporaryFibPeak; // nodeId -> peak temporary FIB size

// -------------------------------------------------- direct FIB management
// (mobile node runs a plain NFD stack; its single default route must follow
//  the active access link).
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

// ------------------------------------------------------- temporary FIB sampling
static void TemporaryFibSampleTick(Time until)
{
  for (uint32_t i = 0; i < g_backboneNodes.GetN(); ++i)
  {
    Ptr<Node> n = g_backboneNodes.Get(i);
    auto l3 = n->GetObject<ndn::L3Protocol>();
    if (!l3 || !l3->isUsingStatimEngine())
      continue;
    auto engine = l3->getStatimEngine();
    if (!engine)
      continue;
    size_t sz = engine->getTemporaryFibSize();
    auto& peak = g_temporaryFibPeak[n->GetId()];
    if (sz > peak)
      peak = sz;
  }
  if (Simulator::Now() < until)
  {
    Simulator::Schedule(MilliSeconds(10), &TemporaryFibSampleTick, until);
  }
}

// ------------------------------------------------------------- handover
static void InitialAttach(Ptr<Node> apOld, Ptr<Node> apNew)
{
  // start attached to R3: fail the R6 link, point the mobile FIB at R3
  wiredhandover::FailP2PLink(g_mobileNode, apNew);
  nfd::FaceId fid = FindFaceIdToNode(g_mobileNode, apOld);
  if (fid != nfd::face::INVALID_FACEID)
    DirectFibSet(g_mobileNode, ndn::Name("/"), fid);

  ndn::StatimMobile* mobileApp =
      dynamic_cast<ndn::StatimMobile*>(&(*g_mobileNode->GetApplication(0)));
  if (mobileApp)
  {
    mobileApp->m_current = apOld->GetId();
    mobileApp->OnAssociation();
  }
  g_oldApId = apOld->GetId();
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

  ndn::StatimMobile* mobileApp =
      dynamic_cast<ndn::StatimMobile*>(&(*g_mobileNode->GetApplication(0)));
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

// --------------------------------------------- final flow-FIB consistency
// Record whether the expected post-handover flow route remains installed
// after completion-notice loss and temporary-state withdrawal.
static int FindEnginePortToNode(Ptr<Node> fromNode, Ptr<Node> toNode)
{
  auto l3 = fromNode->GetObject<ndn::L3Protocol>();
  if (!l3 || !l3->isUsingStatimEngine())
    return -1;
  auto engine = l3->getStatimEngine();
  if (!engine)
    return -1;
  for (uint32_t d = 0; d < fromNode->GetNDevices(); ++d)
  {
    Ptr<PointToPointNetDevice> ppd = DynamicCast<PointToPointNetDevice>(fromNode->GetDevice(d));
    if (!ppd)
      continue;
    Ptr<Channel> ch = ppd->GetChannel();
    if (!ch)
      continue;
    Ptr<NetDevice> remote = ch->GetDevice(0);
    if (remote->GetNode() == fromNode)
      remote = ch->GetDevice(1);
    if (remote->GetNode() == toNode)
    {
      return engine->getPortTable().getPort(ppd);
    }
  }
  return -1;
}

static void CheckFinalFib(Ptr<Node> node, const char* label, Ptr<Node> expectNext,
                          const std::string& prefixStr)
{
  auto l3 = node->GetObject<ndn::L3Protocol>();
  if (!l3 || !l3->isUsingStatimEngine())
    return;
  auto engine = l3->getStatimEngine();
  if (!engine)
    return;

  ndn::Name prefix(prefixStr);
  ndn::statim::PacketHeader h(prefix);
  h.setType(ndn::statim::PacketType::INTEREST);
  h.setOutPorts(0);
  h.setPacketInPort(60000); // Diagnostic marker outside this scenario's port set.
  auto res = engine->getSwitch().processPacket(h);

  int expected = FindEnginePortToNode(node, expectNext);
  bool ok = false;
  std::string ports;
  for (uint16_t p : res.outputPorts)
  {
    ports += std::to_string(p) + " ";
    if ((int)p == expected)
      ok = true;
  }
  std::cerr << "[FIB_FINAL] node=" << label << " prefix=" << prefixStr << " outPorts=[ " << ports
            << "] expectedPort=" << expected << " ok=" << (ok ? 1 : 0) << std::endl;
}

// ------------------------------------------------------------ counters
static void PrintCounters()
{
  for (uint32_t i = 0; i < g_backboneNodes.GetN(); ++i)
  {
    Ptr<Node> n = g_backboneNodes.Get(i);
    auto l3 = n->GetObject<ndn::L3Protocol>();
    if (!l3 || !l3->isUsingStatimEngine())
      continue;
    auto engine = l3->getStatimEngine();
    if (!engine)
      continue;
    const auto& ec = engine->getCounters();
    const auto& tc = engine->getStatefulModule().getTemporaryController().getCounters();
    const auto& cc = engine->getCentralController().getCounters();
    std::cerr << "[STATIM_COUNTERS] node=R" << (n->GetId() + 1)
              << " traceUpdates=" << ec.traceUpdatesHandled << " tempUpdates=" << tc.tempUpdates
              << " controlPkts=" << tc.controlPackets << " syncCompletions=" << tc.syncCompletions
              << " packetIns=" << cc.packetIns << " flowMods=" << cc.flowModsApplied
              << " flowTableUpdates=" << ec.flowTableUpdatesApplied
              << " pullInvocations=" << ec.pullInvocations << " pullsSent=" << ec.pendingPullsSent
              << " temporaryFibInterestTransmissions=" << ec.temporaryFibInterestTransmissions
              << " flowTableFibInterestTransmissions=" << ec.flowTableFibInterestTransmissions
              << " controllerRetries=" << tc.controllerRetries << " temporaryRouteWithdrawals=" << tc.temporaryRouteWithdrawals
              << " acksDropped=" << cc.syncAcksDropped << std::endl;
    std::cerr << "[TEMPORARY_FIB_PEAK] node=R" << (n->GetId() + 1) << " peak=" << g_temporaryFibPeak[n->GetId()]
              << std::endl;
  }
}

int main(int argc, char* argv[])
{
  Config::SetDefault("ns3::PointToPointNetDevice::DataRate", StringValue("1Gbps"));
  Config::SetDefault("ns3::PointToPointChannel::Delay", StringValue("10ms"));
  Config::SetDefault("ns3::DropTailQueue::MaxPackets", StringValue("10000"));

  uint32_t run = 1;
  double stopTime = 32.0;
  double controllerDelay = 0.0; // T_c (seconds) — key experimental variable
  double statefulDelay = 0.0;   // per-packet stateful-module cost (seconds)
  bool enableTemporaryFib = true;
  bool enableInterestReforwarding = true;
  int interestReforwardingLimit = 1000;          // budget parity with KITE's per-invocation cap
  bool consumerRetransmissions = true;           // true: consumer retransmission timer is enabled
  double consumerCbrFreq = 1000.0; // r
  double interestLifetimeSeconds = 2;
  double initialRttEstimate = 2;
  double traceLifeTime = 2;
  double refreshInterval = 2;
  int payloadSize = 1024;
  double hoBase = 20.0;
  double hoJitter = 1.0;
  double windowLen = 4.0;
  std::string appDelayFile = "";
  double controlLossRate = 0.0; // P(drop completion notice / SyncAck)
  double controllerRetryTimeout = -1.0;   // s; <0 = auto (2*T_c + 0.05s) when fault injection is on, else off
  int controllerRetryLimit = 3;
  bool generationGuard = false;

  CommandLine cmd;
  cmd.AddValue("run", "ns-3 random-stream run number (positive integer)", run);
  cmd.AddValue("stop", "stop time (s)", stopTime);
  cmd.AddValue("controllerDelay",
               "decision-to-effect controller delay T_c (seconds)",
               controllerDelay);
  cmd.AddValue("statefulDelay", "stateful-module per-packet cost (s)", statefulDelay);
  cmd.AddValue("enableTemporaryFib",
               "enable forwarding through the temporary FIB after local route updates",
               enableTemporaryFib);
  cmd.AddValue("enableInterestReforwarding",
               "reforward pending Interests when a newer Trace updates the route",
               enableInterestReforwarding);
  cmd.AddValue("interestReforwardingLimit",
               "maximum pending Interests reforwarded per accepted Trace update",
               interestReforwardingLimit);
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
  cmd.AddValue("controlLossRate", "P(drop the completion notice / SyncAck)", controlLossRate);
  cmd.AddValue("controllerRetryTimeout",
               "controller-request retry interval (seconds); negative=automatic, zero=disabled",
               controllerRetryTimeout);
  cmd.AddValue("controllerRetryLimit",
               "maximum retries after the initial controller request",
               controllerRetryLimit);
  cmd.AddValue("generationGuard", "retain each prefix latest accepted sequence number to reject delayed updates",
               generationGuard);
  cmd.Parse(argc, argv);
  if (interestLifetimeSeconds <= 0 || initialRttEstimate <= 0)
  {
    NS_FATAL_ERROR("Interest lifetime and initial RTT estimate must be positive");
  }

  // Default timeout: twice controllerDelay plus 0.05 s.
  if (controllerRetryTimeout < 0)
  {
    controllerRetryTimeout = (controlLossRate > 0) ? (2.0 * controllerDelay + 0.05) : 0.0;
  }

  Config::SetGlobal("RngRun", IntegerValue(run));
  Config::SetDefault("ns3::ndn::RttEstimator::InitialEstimation", TimeValue(Seconds(initialRttEstimate)));

  // handover instant derived from the run seed (start-offset randomization)
  Ptr<UniformRandomVariable> hoRand = CreateObject<UniformRandomVariable>();
  hoRand->SetStream(9000);
  double tHo = hoBase + hoRand->GetValue(0.0, 1.0) * hoJitter;

  std::cerr << std::defaultfloat << std::setprecision(17)
            << "[CONFIG] scenario=statim-grid run=" << run << " controllerDelay=" << controllerDelay
            << " statefulDelay=" << statefulDelay << " enableTemporaryFib=" << (enableTemporaryFib ? 1 : 0)
            << " enableInterestReforwarding=" << (enableInterestReforwarding ? 1 : 0) << " interestReforwardingLimit=" << interestReforwardingLimit
            << " interestLifetime=" << interestLifetimeSeconds << " initialRttEstimate=" << initialRttEstimate
            << " consumerRetransmissions=" << (consumerRetransmissions ? 1 : 0) << " r=" << consumerCbrFreq
            << " t_ho=" << tHo << " windowLen=" << windowLen
            << " stop=" << stopTime << " controlLossRate=" << controlLossRate
            << " controllerRetryTimeout=" << controllerRetryTimeout << " controllerRetryLimit=" << controllerRetryLimit
            << " generationGuard=" << (generationGuard ? 1 : 0)
            << " traceLifeTime=" << traceLifeTime << " refreshInterval=" << refreshInterval
            << std::endl;

  // ---------------- topology: R1..R6 grid + consumer + RV ----------------
  NodeContainer nodes;
  AnnotatedTopologyReader topologyReader("", 25);
  topologyReader.SetFileName("src/ndnSIM/statim/scenarios/common/grid-topology.txt");
  nodes = topologyReader.Read(); // R1=0 R2=1 R3=2 R4=3 R5=4 R6=5
  nodes.Create(2);               // consumer=6, RV=7

  PointToPointHelper p2p;
  p2p.Install(nodes.Get(6), nodes.Get(3)); // consumer -- R4
  p2p.Install(nodes.Get(7), nodes.Get(0)); // RV -- R1

  // Fixed node positions for topology visualization.
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

  // ---------------- STATIM stack on the 6 forwarders -----------------------
  for (int i = 0; i < 6; ++i)
    g_backboneNodes.Add(nodes.Get(i));

  ndn::StatimHelper statimHelper;
  statimHelper.SetControllerDelay(Seconds(controllerDelay));
  statimHelper.SetEnableInterestReforwarding(enableInterestReforwarding);
  statimHelper.SetEnableTemporaryFib(enableTemporaryFib);
  statimHelper.SetStatefulDelay(Seconds(statefulDelay));
  statimHelper.SetInterestReforwardingLimit(interestReforwardingLimit);
  statimHelper.SetFlowTableFibEntryLifetime(Seconds(traceLifeTime));
  statimHelper.SetTemporaryFibEntryLifetime(Seconds(traceLifeTime));
  statimHelper.SetControllerRetryTimeout(Seconds(controllerRetryTimeout));
  statimHelper.SetControllerRetryLimit(controllerRetryLimit);
  statimHelper.SetGenerationGuardEnabled(generationGuard);
  statimHelper.SetSyncAckLossRate(controlLossRate);
  statimHelper.Install(g_backboneNodes);

  // plain NDN stack on consumer / RV / mobile (STATIM header added at the edge)
  ndn::StackHelper ndnHelper;
  ndnHelper.SetDefaultRoutes(true);
  ndnHelper.SetStackAttributes("EnableStatimPacket", "true");
  ndnHelper.Install(nodes.Get(6));
  ndnHelper.Install(nodes.Get(7));
  ndnHelper.SetDefaultRoutes(false); // mobile FIB is managed via DirectFibSet
  ndnHelper.SetOldContentStore("ns3::ndn::cs::Nocache");
  ndnHelper.Install(mobileNodes);

  // Keep the mobile out of the static Dijkstra graph.
  g_mobileNode->AggregateObject(CreateObject<ndn::GlobalRouter>());

  std::string dataPrefix = "/alice/photo";
  std::string rvPrefix = "/rv";

  ndn::GlobalRoutingHelper ndnGlobalRoutingHelper;
  ndnGlobalRoutingHelper.Install(nodes);
  ndnGlobalRoutingHelper.AddOrigins(rvPrefix, nodes.Get(7));

  // ---------------- applications ----------------------------------------
  ndn::AppHelper consumerHelper("ns3::ndn::StatimConsumer");
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

  ndn::AppHelper mobileHelper("ns3::ndn::StatimMobile");
  mobileHelper.SetPrefix(rvPrefix + dataPrefix);
  mobileHelper.SetAttribute("RvPrefix", StringValue(rvPrefix));
  mobileHelper.SetAttribute("DataPrefix", StringValue(dataPrefix));
  mobileHelper.SetAttribute("PayloadSize", StringValue(std::to_string(payloadSize)));
  mobileHelper.SetAttribute("TraceLifetime", TimeValue(Seconds(traceLifeTime)));
  mobileHelper.SetAttribute("RefreshInterval", TimeValue(Seconds(refreshInterval)));
  // Match the KITE adapter's 10 ms association-to-TI dispatch delay.
  mobileHelper.SetAttribute("TraceDelay", StringValue("0.01s"));
  ApplicationContainer mobileApp = mobileHelper.Install(g_mobileNode);
  mobileApp.Stop(Seconds(stopTime - 1.0));

  ndn::StatimHelper::CalculateStatimRoutes();

  // ---------------- schedule: attach, handover, sampling ----------------
  Simulator::Schedule(Seconds(0.2), &InitialAttach, nodes.Get(2), nodes.Get(5));
  Simulator::Schedule(Seconds(tHo), &DoHandover, nodes.Get(2), nodes.Get(5));
  Simulator::Schedule(Seconds(tHo - 0.5), &TemporaryFibSampleTick, Seconds(tHo + controllerDelay + 3.0));

  if (!appDelayFile.empty())
  {
    ndn::AppDelayTracer::InstallAll(appDelayFile);
  }

  Simulator::Stop(Seconds(stopTime));
  Simulator::Run();

  PrintCounters();

  // port map for interpreting FIB/counters output
  {
    const char* names[6] = {"R1", "R2", "R3", "R4", "R5", "R6"};
    for (int i = 0; i < 6; ++i)
    {
      std::cerr << "[PORTMAP] node=" << names[i];
      for (int j = 0; j < 6; ++j)
      {
        if (j == i)
          continue;
        int p = FindEnginePortToNode(nodes.Get(i), nodes.Get(j));
        if (p >= 0)
          std::cerr << " " << names[j] << "=" << p;
      }
      int pm = FindEnginePortToNode(nodes.Get(i), g_mobileNode);
      if (pm >= 0)
        std::cerr << " MP=" << pm;
      int pc = FindEnginePortToNode(nodes.Get(i), nodes.Get(6));
      if (pc >= 0)
        std::cerr << " CONSUMER=" << pc;
      int pr = FindEnginePortToNode(nodes.Get(i), nodes.Get(7));
      if (pr >= 0)
        std::cerr << " RV=" << pr;
      std::cerr << std::endl;
    }
  }

  // The checks below expect the post-handover trace path R6-R5-R2-R1, with
  // data-prefix routes R1->R2, R2->R5, R5->R6, and R6->MP. At R3 they expect
  // the stale data-prefix route to expire and lookup to use /rv toward R2.
  CheckFinalFib(nodes.Get(0), "R1", nodes.Get(1), rvPrefix + dataPrefix);
  CheckFinalFib(nodes.Get(1), "R2", nodes.Get(4), rvPrefix + dataPrefix);
  CheckFinalFib(nodes.Get(4), "R5", nodes.Get(5), rvPrefix + dataPrefix);
  CheckFinalFib(nodes.Get(5), "R6", g_mobileNode, rvPrefix + dataPrefix);
  CheckFinalFib(nodes.Get(2), "R3", nodes.Get(1), rvPrefix + dataPrefix);

  Simulator::Destroy();
  return 0;
}

} // namespace ns3

int main(int argc, char* argv[])
{
  return ns3::main(argc, argv);
}
