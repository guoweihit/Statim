// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/core-module.h"
#include "ns3/network-module.h"
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

#include "ns3/ndnSIM/NFD/daemon/table/fib-entry.hpp"
#include "ns3/ndnSIM/NFD/daemon/table/fib.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

NS_LOG_COMPONENT_DEFINE("statim.path");

namespace ns3
{

namespace
{

struct CounterSnapshot
{
  uint64_t traceInterests;
  uint64_t traceUpdates;
  uint64_t tempUpdates;
  uint64_t controls;
  uint64_t packetIns;
  uint64_t flowMods;
  uint64_t temporaryFibInterestTransmissions;
  uint64_t flowTableFibInterestTransmissions;

  CounterSnapshot()
      : traceInterests(0), traceUpdates(0), tempUpdates(0), controls(0), packetIns(0), flowMods(0),
        temporaryFibInterestTransmissions(0), flowTableFibInterestTransmissions(0)
  {
  }
};

Ptr<Node> g_mobileNode;
NodeContainer g_forwarders;
std::vector<std::string> g_labels;
std::vector<CounterSnapshot> g_beforeHandover;
std::map<uint32_t, size_t> g_temporaryFibPeak;
uint32_t g_tracePathHops = 0;

std::shared_ptr<ndn::statim::ForwardingEngine> GetEngine(Ptr<Node> node)
{
  auto l3 = node->GetObject<ndn::L3Protocol>();
  if (!l3 || !l3->isUsingStatimEngine())
  {
    return nullptr;
  }
  return l3->getStatimEngine();
}

CounterSnapshot ReadCounters(Ptr<Node> node)
{
  CounterSnapshot out;
  auto engine = GetEngine(node);
  if (!engine)
  {
    return out;
  }

  const auto& ec = engine->getCounters();
  const auto& tc = engine->getStatefulModule().getTemporaryController().getCounters();
  const auto& cc = engine->getCentralController().getCounters();
  out.traceInterests = ec.traceInterestsHandled;
  out.traceUpdates = ec.traceUpdatesHandled;
  out.tempUpdates = tc.tempUpdates;
  out.controls = tc.controlPackets;
  out.packetIns = cc.packetIns;
  out.flowMods = cc.flowModsApplied;
  out.temporaryFibInterestTransmissions = ec.temporaryFibInterestTransmissions;
  out.flowTableFibInterestTransmissions = ec.flowTableFibInterestTransmissions;
  return out;
}

uint64_t Delta(uint64_t after, uint64_t before)
{
  return after >= before ? after - before : 0;
}

nfd::FaceId FindFaceIdToNode(Ptr<Node> fromNode, Ptr<Node> toNode)
{
  auto l3 = fromNode->GetObject<ndn::L3Protocol>();
  if (!l3)
  {
    return nfd::face::INVALID_FACEID;
  }
  auto forwarder = l3->getForwarder();
  if (!forwarder)
  {
    return nfd::face::INVALID_FACEID;
  }
  for (const auto& face : forwarder->getFaceTable())
  {
    auto transport = dynamic_cast<ndn::NetDeviceTransport*>(face.getTransport());
    if (!transport)
    {
      continue;
    }
    auto device = DynamicCast<PointToPointNetDevice>(transport->GetNetDevice());
    if (!device || !device->GetChannel())
    {
      continue;
    }
    Ptr<NetDevice> remote = device->GetChannel()->GetDevice(0);
    if (remote->GetNode() == fromNode)
    {
      remote = device->GetChannel()->GetDevice(1);
    }
    if (remote->GetNode() == toNode)
    {
      return face.getId();
    }
  }
  return nfd::face::INVALID_FACEID;
}

void DirectFibSet(Ptr<Node> node, const ndn::Name& prefix, nfd::FaceId faceId)
{
  auto forwarder = node->GetObject<ndn::L3Protocol>()->getForwarder();
  nfd::Face* face = forwarder->getFaceTable().get(faceId);
  if (!face)
  {
    return;
  }
  auto result = forwarder->getFib().insert(prefix);
  result.first->addNextHop(*face, 0);
}

void DirectFibClear(Ptr<Node> node, const ndn::Name& prefix)
{
  auto l3 = node->GetObject<ndn::L3Protocol>();
  if (l3 && l3->getForwarder())
  {
    l3->getForwarder()->getFib().erase(prefix);
  }
}

int FindEnginePortToNode(Ptr<Node> fromNode, Ptr<Node> toNode)
{
  auto engine = GetEngine(fromNode);
  if (!engine)
  {
    return -1;
  }
  for (uint32_t d = 0; d < fromNode->GetNDevices(); ++d)
  {
    auto device = DynamicCast<PointToPointNetDevice>(fromNode->GetDevice(d));
    if (!device || !device->GetChannel())
    {
      continue;
    }
    Ptr<NetDevice> remote = device->GetChannel()->GetDevice(0);
    if (remote->GetNode() == fromNode)
    {
      remote = device->GetChannel()->GetDevice(1);
    }
    if (remote->GetNode() == toNode)
    {
      return engine->getPortTable().getPort(device);
    }
  }
  return -1;
}

void CaptureHandoverBoundary()
{
  g_beforeHandover.clear();
  for (uint32_t i = 0; i < g_forwarders.GetN(); ++i)
  {
    g_beforeHandover.push_back(ReadCounters(g_forwarders.Get(i)));
  }
  std::cerr << "[PATH_BOUNDARY] t=" << std::fixed << std::setprecision(9)
            << Simulator::Now().GetSeconds() << " captured=1" << std::endl;
}

void InitialAttach(Ptr<Node> oldAp, Ptr<Node> newAp)
{
  wiredhandover::FailP2PLink(g_mobileNode, newAp);
  nfd::FaceId face = FindFaceIdToNode(g_mobileNode, oldAp);
  if (face != nfd::face::INVALID_FACEID)
  {
    DirectFibSet(g_mobileNode, ndn::Name("/"), face);
  }

  auto mobile = dynamic_cast<ndn::StatimMobile*>(&(*g_mobileNode->GetApplication(0)));
  if (mobile)
  {
    mobile->m_current = oldAp->GetId();
    mobile->OnAssociation();
  }
  std::cerr << "[ATTACH] t=" << std::fixed << std::setprecision(9) << Simulator::Now().GetSeconds()
            << " oldApNodeId=" << oldAp->GetId() << std::endl;
}

void DoHandover(Ptr<Node> oldAp, Ptr<Node> newAp)
{
  CaptureHandoverBoundary();
  wiredhandover::FailP2PLink(g_mobileNode, oldAp);
  wiredhandover::UpP2PLink(g_mobileNode, newAp);

  DirectFibClear(g_mobileNode, ndn::Name("/"));
  nfd::FaceId face = FindFaceIdToNode(g_mobileNode, newAp);
  if (face != nfd::face::INVALID_FACEID)
  {
    DirectFibSet(g_mobileNode, ndn::Name("/"), face);
  }

  auto mobile = dynamic_cast<ndn::StatimMobile*>(&(*g_mobileNode->GetApplication(0)));
  if (mobile)
  {
    mobile->m_current = newAp->GetId();
    mobile->OnAssociation();
    mobile->MarkHandover(Simulator::Now());
  }
  std::cerr << "[HANDOVER] t=" << std::fixed << std::setprecision(9)
            << Simulator::Now().GetSeconds() << " oldApNodeId=" << oldAp->GetId()
            << " newApNodeId=" << newAp->GetId() << std::endl;
}

void TemporaryFibSampleTick(Time until)
{
  for (uint32_t i = 0; i < g_forwarders.GetN(); ++i)
  {
    auto engine = GetEngine(g_forwarders.Get(i));
    if (!engine)
    {
      continue;
    }
    size_t size = engine->getTemporaryFibSize();
    size_t& peak = g_temporaryFibPeak[g_forwarders.Get(i)->GetId()];
    if (size > peak)
    {
      peak = size;
    }
  }
  if (Simulator::Now() < until)
  {
    Simulator::Schedule(MilliSeconds(5), &TemporaryFibSampleTick, until);
  }
}

void PrintHandoverDeltas()
{
  uint64_t ti = 0;
  uint64_t td = 0;
  uint64_t tempUpdates = 0;
  uint64_t controls = 0;
  uint64_t packetIns = 0;
  uint64_t flowMods = 0;
  uint64_t temporaryFibInterestTransmissions = 0;
  uint64_t flowTableFibInterestTransmissions = 0;

  for (uint32_t i = 0; i < g_forwarders.GetN(); ++i)
  {
    CounterSnapshot before;
    if (i < g_beforeHandover.size())
    {
      before = g_beforeHandover[i];
    }
    CounterSnapshot after = ReadCounters(g_forwarders.Get(i));

    uint64_t dTi = Delta(after.traceInterests, before.traceInterests);
    uint64_t dTd = Delta(after.traceUpdates, before.traceUpdates);
    uint64_t dTu = Delta(after.tempUpdates, before.tempUpdates);
    uint64_t dCo = Delta(after.controls, before.controls);
    uint64_t dPi = Delta(after.packetIns, before.packetIns);
    uint64_t dFm = Delta(after.flowMods, before.flowMods);
    uint64_t dTf = Delta(after.temporaryFibInterestTransmissions,
                        before.temporaryFibInterestTransmissions);
    uint64_t dFf = Delta(after.flowTableFibInterestTransmissions,
                        before.flowTableFibInterestTransmissions);

    ti += dTi;
    td += dTd;
    tempUpdates += dTu;
    controls += dCo;
    packetIns += dPi;
    flowMods += dFm;
    temporaryFibInterestTransmissions += dTf;
    flowTableFibInterestTransmissions += dFf;

    std::cerr << "[PATH_NODE] label=" << g_labels[i] << " nodeId=" << g_forwarders.Get(i)->GetId()
              << " traceTI=" << dTi << " traceTD=" << dTd << " tempUpdates=" << dTu
              << " controls=" << dCo << " packetIns=" << dPi << " flowMods=" << dFm
              << " temporaryFibInterestTransmissions=" << dTf
              << " flowTableFibInterestTransmissions=" << dFf
              << " temporaryFibPeak=" << g_temporaryFibPeak[g_forwarders.Get(i)->GetId()] << std::endl;
  }

  uint64_t tiBackboneHops = ti > 0 ? ti - 1 : 0;
  uint64_t tdBackboneHops = td > 0 ? td - 1 : 0;
  std::cerr << "[PATH_TOTAL] configuredTracePathHops=" << g_tracePathHops
            << " traceTiRouterVisits=" << ti << " traceTdRouterVisits=" << td
            << " traceTiBackboneHops=" << tiBackboneHops
            << " traceTdBackboneHops=" << tdBackboneHops << " tempUpdates=" << tempUpdates
            << " controls=" << controls << " packetIns=" << packetIns << " flowMods=" << flowMods
            << " temporaryFibInterestTransmissions=" << temporaryFibInterestTransmissions
            << " flowTableFibInterestTransmissions=" << flowTableFibInterestTransmissions
            << " pathCountOk="
            << ((tiBackboneHops == g_tracePathHops && tdBackboneHops == g_tracePathHops) ? 1 : 0)
            << std::endl;
}

void CheckFinalFlowPath(const NodeContainer& chain, Ptr<Node> mobile, const std::string& prefix)
{
  uint32_t checked = 0;
  uint32_t okCount = 0;
  for (uint32_t i = 0; i < chain.GetN(); ++i)
  {
    Ptr<Node> expected = i + 1 < chain.GetN() ? chain.Get(i + 1) : mobile;
    auto engine = GetEngine(chain.Get(i));
    if (!engine)
    {
      continue;
    }

    ndn::statim::PacketHeader header{ndn::Name(prefix)};
    header.setType(ndn::statim::PacketType::INTEREST);
    header.setOutPorts(0);
    header.setPacketInPort(60000);
    auto result = engine->getSwitch().processPacket(header);
    int expectedPort = FindEnginePortToNode(chain.Get(i), expected);
    bool ok = false;
    for (uint16_t port : result.outputPorts)
    {
      if (static_cast<int>(port) == expectedPort)
      {
        ok = true;
      }
    }
    ++checked;
    if (ok)
    {
      ++okCount;
    }
    std::cerr << "[PATH_FIB_NODE] label=R" << i << " expectedPort=" << expectedPort
              << " ok=" << (ok ? 1 : 0) << std::endl;
  }
  std::cerr << "[PATH_FIB_TOTAL] checked=" << checked << " ok=" << okCount
            << " allOk=" << (checked == okCount ? 1 : 0) << std::endl;
}

} // namespace

int main(int argc, char* argv[])
{
  Config::SetDefault("ns3::PointToPointNetDevice::DataRate", StringValue("1Gbps"));
  Config::SetDefault("ns3::PointToPointChannel::Delay", StringValue("10ms"));
  Config::SetDefault("ns3::DropTailQueue::MaxPackets", StringValue("10000"));

  uint32_t run = 1;
  uint32_t tracePathHops = 2;
  std::string consumerJoin = "rendezvous";
  double stopTime = 18.0;
  double controllerDelay = 0.0;
  double statefulDelay = 0.0;
  bool enableTemporaryFib = true;
  bool enableInterestReforwarding = false;
  bool consumerRetransmissions = true;
  double consumerCbrFreq = 1000.0;
  double interestLifetimeSeconds = 2;
  double initialRttEstimate = 2;
  double traceLifeTime = 30.0;
  double refreshInterval = 100.0;
  int payloadSize = 1024;
  double hoBase = 10.0;
  double hoJitter = 0.5;
  double windowLen = 4.0;

  CommandLine cmd;
  cmd.AddValue("run", "ns-3 random-stream run number (positive integer)", run);
  cmd.AddValue("tracePathHops", "router-to-router hops R0..RH (minimum 2)", tracePathHops);
  cmd.AddValue("consumerJoin",
               "consumer convergence position: rendezvous (index 0) or middle (hops/2)",
               consumerJoin);
  cmd.AddValue("stop", "simulation stop time (s)", stopTime);
  cmd.AddValue("controllerDelay", "deterministic decision-to-effect controller delay T_c (seconds)",
               controllerDelay);
  cmd.AddValue("statefulDelay", "per-packet stateful-module cost (s)", statefulDelay);
  cmd.AddValue("enableTemporaryFib",
               "enable forwarding through the temporary FIB after local route updates",
               enableTemporaryFib);
  cmd.AddValue("enableInterestReforwarding",
               "reforward pending Interests when a newer Trace updates the route",
               enableInterestReforwarding);
  cmd.AddValue("consumerRetransmissions",
               "enable consumer Interest retransmissions during the simulation",
               consumerRetransmissions);
  cmd.AddValue("consumerCbrFreq", "consumer Interest rate (1/s)", consumerCbrFreq);
  cmd.AddValue("interestLifetime", "Interest packet lifetime (seconds > 0)", interestLifetimeSeconds);
  cmd.AddValue("initialRttEstimate", "Initial RTT estimate before RTT samples (seconds > 0)", initialRttEstimate);
  cmd.AddValue("traceLifeTime", "Trace/FIB entry lifetime (s)", traceLifeTime);
  cmd.AddValue("refreshInterval", "periodic Trace refresh interval (s)", refreshInterval);
  cmd.AddValue("payloadSize", "Data payload bytes", payloadSize);
  cmd.AddValue("hoBase", "handover base instant (s)", hoBase);
  cmd.AddValue("hoJitter", "handover start-time jitter width (s)", hoJitter);
  cmd.AddValue("windowLen", "post-handover loss window (s)", windowLen);
  cmd.Parse(argc, argv);
  if (interestLifetimeSeconds <= 0 || initialRttEstimate <= 0)
  {
    NS_FATAL_ERROR("Interest lifetime and initial RTT estimate must be positive");
  }

  if (tracePathHops < 2)
  {
    NS_FATAL_ERROR("tracePathHops must be at least 2");
  }
  if (consumerJoin != "rendezvous" && consumerJoin != "middle")
  {
    NS_FATAL_ERROR("consumerJoin must be rendezvous or middle");
  }
  if (refreshInterval <= stopTime)
  {
    NS_FATAL_ERROR("Path-isolation experiment requires refreshInterval > stop time");
  }
  if (traceLifeTime <= stopTime)
  {
    NS_FATAL_ERROR("Path-isolation experiment requires traceLifeTime > stop time");
  }

  g_tracePathHops = tracePathHops;
  Config::SetGlobal("RngRun", IntegerValue(run));
  Config::SetDefault("ns3::ndn::RttEstimator::InitialEstimation", TimeValue(Seconds(initialRttEstimate)));

  Ptr<UniformRandomVariable> hoRand = CreateObject<UniformRandomVariable>();
  hoRand->SetStream(9000);
  double tHo = hoBase + hoRand->GetValue(0.0, 1.0) * hoJitter;
  uint32_t joinIndex = consumerJoin == "rendezvous" ? 0 : tracePathHops / 2;

  NodeContainer chain;
  chain.Create(tracePathHops + 1); // R0..RH
  NodeContainer oldAccess;
  oldAccess.Create(1); // one-hop branch at R0
  NodeContainer consumer;
  consumer.Create(1);
  NodeContainer rv;
  rv.Create(1);
  NodeContainer mobile;
  mobile.Create(1);
  g_mobileNode = mobile.Get(0);

  PointToPointHelper p2p;
  for (uint32_t i = 0; i < tracePathHops; ++i)
  {
    p2p.Install(chain.Get(i), chain.Get(i + 1));
  }
  p2p.Install(chain.Get(0), oldAccess.Get(0));
  p2p.Install(chain.Get(joinIndex), consumer.Get(0));
  p2p.Install(chain.Get(0), rv.Get(0));

  std::vector<Ptr<Node>> accessNodes;
  accessNodes.push_back(oldAccess.Get(0));
  accessNodes.push_back(chain.Get(tracePathHops));
  wiredhandover::CreateLinks(g_mobileNode, accessNodes, "1Gbps", "10ms");

  g_forwarders.Add(chain);
  g_forwarders.Add(oldAccess);
  for (uint32_t i = 0; i < chain.GetN(); ++i)
  {
    std::ostringstream label;
    label << "R" << i;
    g_labels.push_back(label.str());
  }
  g_labels.push_back("OLD");

  ndn::StatimHelper statimHelper;
  statimHelper.SetControllerDelay(Seconds(controllerDelay));
  statimHelper.SetEnableInterestReforwarding(enableInterestReforwarding);
  statimHelper.SetEnableTemporaryFib(enableTemporaryFib);
  statimHelper.SetExperimentTelemetryEnabled(true);
  statimHelper.SetStatefulDelay(Seconds(statefulDelay));
  statimHelper.SetInterestReforwardingLimit(1000);
  statimHelper.SetFlowTableFibEntryLifetime(Seconds(traceLifeTime));
  statimHelper.SetTemporaryFibEntryLifetime(Seconds(traceLifeTime));
  statimHelper.Install(g_forwarders);

  ndn::StackHelper ndnHelper;
  ndnHelper.SetDefaultRoutes(true);
  ndnHelper.SetStackAttributes("EnableStatimPacket", "true");
  ndnHelper.Install(consumer);
  ndnHelper.Install(rv);
  ndnHelper.SetDefaultRoutes(false);
  ndnHelper.SetOldContentStore("ns3::ndn::cs::Nocache");
  ndnHelper.Install(mobile);
  g_mobileNode->AggregateObject(CreateObject<ndn::GlobalRouter>());

  NodeContainer routedNodes;
  routedNodes.Add(g_forwarders);
  routedNodes.Add(consumer);
  routedNodes.Add(rv);
  ndn::GlobalRoutingHelper routing;
  routing.Install(routedNodes);
  const std::string dataPrefix = "/alice/photo";
  const std::string rvPrefix = "/rv";
  routing.AddOrigins(rvPrefix, rv.Get(0));

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
  auto consumerApp = consumerHelper.Install(consumer);
  consumerApp.Start(Seconds(1.0));
  consumerApp.Stop(Seconds(stopTime - 2.0));

  ndn::AppHelper rvHelper("ns3::ndn::KiteRv");
  rvHelper.SetAttribute("RvPrefix", StringValue(rvPrefix));
  rvHelper.Install(rv);

  ndn::AppHelper mobileHelper("ns3::ndn::StatimMobile");
  mobileHelper.SetPrefix(rvPrefix + dataPrefix);
  mobileHelper.SetAttribute("RvPrefix", StringValue(rvPrefix));
  mobileHelper.SetAttribute("DataPrefix", StringValue(dataPrefix));
  mobileHelper.SetAttribute("PayloadSize", StringValue(std::to_string(payloadSize)));
  mobileHelper.SetAttribute("TraceLifetime", TimeValue(Seconds(traceLifeTime)));
  mobileHelper.SetAttribute("RefreshInterval", TimeValue(Seconds(refreshInterval)));
  mobileHelper.SetAttribute("TraceDelay", StringValue("0.01s"));
  auto mobileApp = mobileHelper.Install(mobile);
  mobileApp.Stop(Seconds(stopTime - 1.0));

  ndn::StatimHelper::CalculateStatimRoutes();

  std::ostringstream path;
  for (uint32_t i = 0; i <= tracePathHops; ++i)
  {
    if (i != 0)
    {
      path << ">";
    }
    path << "R" << i;
  }
  std::cerr << std::defaultfloat << std::setprecision(17)
            << "[CONFIG] scenario=statim-path"
            << " run=" << run << " tracePathHops=" << tracePathHops
            << " consumerJoin=" << consumerJoin << " joinIndex=" << joinIndex
            << " controllerDelay=" << controllerDelay
            << " statefulDelay=" << statefulDelay
            << " controllerDelayModel=scheduled-decision-to-effect"
            << " enableTemporaryFib=" << (enableTemporaryFib ? 1 : 0) << " enableInterestReforwarding=" << (enableInterestReforwarding ? 1 : 0)
            << " interestLifetime=" << interestLifetimeSeconds << " initialRttEstimate=" << initialRttEstimate
            << " consumerRetransmissions=" << (consumerRetransmissions ? 1 : 0) << " r=" << consumerCbrFreq
            << " t_ho=" << tHo << " stop=" << stopTime << " windowLen=" << windowLen
            << " refreshInterval=" << refreshInterval << " traceLifeTime=" << traceLifeTime
            << std::endl;
  std::cerr << "[PATH_TOPOLOGY] traceChain=" << path.str() << " oldBranch=R0>OLD"
            << " consumerAt=R" << joinIndex << " rvAt=R0"
            << " newAp=R" << tracePathHops << std::endl;
  for (uint32_t i = 0; i < g_forwarders.GetN(); ++i)
  {
    std::cerr << "[PATH_NODEMAP] nodeId=" << g_forwarders.Get(i)->GetId()
              << " label=" << g_labels[i] << std::endl;
  }

  Simulator::Schedule(Seconds(0.2), &InitialAttach, oldAccess.Get(0), chain.Get(tracePathHops));
  Simulator::Schedule(Seconds(tHo), &DoHandover, oldAccess.Get(0), chain.Get(tracePathHops));
  Simulator::Schedule(Seconds(tHo - 0.25), &TemporaryFibSampleTick, Seconds(stopTime - 0.1));

  Simulator::Stop(Seconds(stopTime));
  Simulator::Run();

  PrintHandoverDeltas();
  CheckFinalFlowPath(chain, g_mobileNode, rvPrefix + dataPrefix);
  Simulator::Destroy();
  return 0;
}

} // namespace ns3

int main(int argc, char* argv[])
{
  return ns3::main(argc, argv);
}
