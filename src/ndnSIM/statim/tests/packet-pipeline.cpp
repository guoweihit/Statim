// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/double.h"
#include "ns3/integer.h"
#include "ns3/ndnSIM/apps/kite-pull-consumer.hpp"
#include "ns3/ndnSIM/helper/ndn-app-helper.hpp"
#include "ns3/ndnSIM/helper/ndn-stack-helper.hpp"
#include "ns3/ndnSIM/model/ndn-block-header.hpp"
#include "ns3/ndnSIM/statim/apps/statim-consumer.hpp"
#include "ns3/ndnSIM/statim/forwarding-engine.hpp"
#include "ns3/ndnSIM/statim/packet-header.hpp"
#include "ns3/ndnSIM/statim/packet-utils.hpp"
#include "ns3/ndnSIM/statim/pipeline/flow-table.hpp"
#include "ns3/ndnSIM/statim/pipeline/switch-pipeline.hpp"
#include "ns3/ndnSIM/statim/stateful/content-store.hpp"
#include "ns3/ndnSIM/statim/stateful/stateful-forwarder.hpp"
#include "ns3/string.h"
#include "ns3/point-to-point-helper.h"

#include <ndn-cxx/encoding/block-helpers.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using namespace ns3::ndn::statim;

size_t checks = 0;

void require(bool condition, const std::string& description)
{
  ++checks;
  if (!condition)
    throw std::runtime_error(description);
}

template <typename Operation>
void requireOutOfRange(Operation operation, const std::string& description)
{
  bool rejected = false;
  try
  {
    operation();
  }
  catch (const std::out_of_range&)
  {
    rejected = true;
  }
  require(rejected, description);
}

std::vector<uint8_t> serialize(const PacketHeader& header)
{
  ns3::Buffer buffer;
  buffer.AddAtStart(header.GetSerializedSize());
  header.Serialize(buffer.Begin());
  std::vector<uint8_t> bytes(header.GetSerializedSize());
  buffer.Begin().Read(bytes.data(), static_cast<uint32_t>(bytes.size()));
  return bytes;
}

PacketHeader parse(const std::vector<uint8_t>& bytes)
{
  ns3::Buffer buffer;
  buffer.AddAtStart(static_cast<uint32_t>(bytes.size()));
  buffer.Begin().Write(bytes.data(), static_cast<uint32_t>(bytes.size()));
  PacketHeader header;
  require(header.Deserialize(buffer.Begin()) == 12, "valid header consumes 12 bytes");
  return header;
}

void testHeader()
{
  PacketHeader empty;
  require(empty.GetSerializedSize() == 12, "wire header remains 12 bytes");
  require(empty.isValid(), "default header is valid");
  require(empty.getNameHashCount() == 0, "default hashes are padding");
  require(serialize(empty) ==
              std::vector<uint8_t>({0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0}),
          "default wire bytes");

  std::vector<uint8_t> wire = {0, 0x81, 0x02, 0x40, 0x01, 0xab, 0xcd, 0x11, 0x22, 0xfe, 0xff, 0x7e};
  const std::array<uint8_t, 5> types = {{0x00, 0x01, 0x02, 0x03, 0x05}};
  for (uint8_t type : types)
  {
    wire[0] = type;
    const auto header = parse(wire);
    require(header.isValid(), "recognized type is valid");
    require(static_cast<uint8_t>(header.getType()) == type, "type round trip");
    require(header.getSwitchId() == 0x7e, "switch ID round trip");
    require(header.getNameHashCount() == 3, "padding follows three supplied hashes");
    require(serialize(header) == wire, "every wire byte survives the round trip");
  }

  PacketHeader fields;
  fields.setType(PacketType::DATA);
  fields.setOutPorts(0x81024001u);
  fields.setPacketInPort(0xabcdu);
  fields.setSwitchId(0x7e);
  auto expected = wire;
  expected[0] = 0x02;
  std::fill(expected.begin() + 7, expected.begin() + 11, 0xff);
  require(serialize(fields) == expected, "port fields use network byte order");

  PacketHeader hashes(::ndn::Name("/alpha/beta/gamma/delta/epsilon"));
  PacketHeader firstFour(::ndn::Name("/alpha/beta/gamma/delta"));
  require(hashes.getNameHashCount() == 4, "names retain at most four component hashes");
  require(hashes.getNameHashes() == firstFour.getNameHashes(), "fifth component is omitted");
  hashes.computeNameHashes(::ndn::Name("/alpha"));
  require(hashes.getNameHashCount() == 1, "recomputation resets old hash slots");
  require(hashes.getNameHashes()[1] == 0xff && hashes.getNameHashes()[3] == 0xff,
          "unused hash slots are 0xff");

  for (uint32_t prefix : {0u, 7u, 20u})
  {
    for (uint32_t available = 0; available < 12; ++available)
    {
      ns3::Buffer buffer;
      buffer.AddAtStart(prefix + available);
      auto start = buffer.Begin();
      start.Next(prefix);
      PacketHeader header = fields;
      require(header.Deserialize(start) == 0, "truncated header reports zero consumed bytes");
      require(serialize(header) == serialize(empty), "truncated input clears previous state");
    }
  }

  ns3::Buffer offsetBuffer;
  offsetBuffer.AddAtStart(21);
  auto offsetStart = offsetBuffer.Begin();
  offsetStart.Next(7);
  offsetStart.Write(wire.data(), static_cast<uint32_t>(wire.size()));
  offsetStart = offsetBuffer.Begin();
  offsetStart.Next(7);
  PacketHeader offsetHeader;
  require(offsetHeader.Deserialize(offsetStart) == 12, "valid header at nonzero iterator offset");
  require(serialize(offsetHeader) == wire, "offset parse ignores trailing payload");

  for (unsigned type = 0; type <= 0xff; ++type)
  {
    if (std::find(types.begin(), types.end(), type) != types.end())
      continue;
    wire[0] = static_cast<uint8_t>(type);
    ns3::Buffer buffer;
    buffer.AddAtStart(12);
    buffer.Begin().Write(wire.data(), 12);
    PacketHeader header;
    require(header.Deserialize(buffer.Begin()) == 0, "unknown type is rejected");
    require(!header.isValid(), "unknown type cannot become an unprocessed packet");
  }
}

::ndn::Data makeTraceWithTtl(const std::vector<uint8_t>& value, bool hasExpiry = true)
{
  // Trace content contains an inner Data TLV, whose Content holds ExpirationPeriod.
  ::ndn::Block innerContent(::ndn::tlv::Content);
  if (hasExpiry)
  {
    innerContent.push_back(::ndn::makeBinaryBlock(0x6d, value.begin(), value.end()));
  }
  innerContent.encode();
  ::ndn::Block innerData(::ndn::tlv::Data);
  innerData.push_back(innerContent);
  innerData.encode();
  ::ndn::Data trace(::ndn::Name("/rv/trace/file/0"));
  trace.setContent(innerData);
  return trace;
}

void testTraceEncoding()
{
  const auto fallback = ::ndn::time::minutes(5);
  require(extractTraceTtl(makeTraceWithTtl({}, false)) == fallback,
          "missing trace TTL uses five-minute fallback");
  require(extractTraceTtl(makeTraceWithTtl({})) == fallback,
          "empty trace TTL uses fallback without reading its value");
  require(extractTraceTtl(makeTraceWithTtl({0x12})) == fallback,
          "one-byte trace TTL uses fallback without reading a second byte");
  require(extractTraceTtl(makeTraceWithTtl({0x12, 0x34, 0x56})) == fallback,
          "three-byte trace TTL is rejected instead of silently truncated");

  require(extractTraceTtl(makeTraceWithTtl({0x00, 0x00})) == ::ndn::time::milliseconds(0),
          "two-byte zero trace TTL remains zero");
  require(extractTraceTtl(makeTraceWithTtl({0x00, 0x01})) == ::ndn::time::milliseconds(1),
          "trace TTL low byte represents milliseconds");
  require(extractTraceTtl(makeTraceWithTtl({0x01, 0x00})) == ::ndn::time::milliseconds(256),
          "trace TTL high byte uses big-endian order");
  require(extractTraceTtl(makeTraceWithTtl({0x12, 0x34})) == ::ndn::time::milliseconds(4660),
          "trace TTL combines both bytes without reversing them");
  require(extractTraceTtl(makeTraceWithTtl({0xff, 0xff})) == ::ndn::time::milliseconds(65535),
          "maximum two-byte trace TTL is unsigned");
}

void testMatchFields()
{
  const std::vector<uint8_t> bytes = {0xab, 0xcd, 0xef, 0x01, 0x23};
  require(MatchField(0, 32, 0xabcdef01u).matches(bytes.data(), bytes.size()), "32-bit extraction");
  require(MatchField(4, 12, 0xbcdu).matches(bytes.data(), bytes.size()), "cross-byte extraction");
  require(MatchField(39, 1, 1).matches(bytes.data(), bytes.size()), "last bit extraction");
  require(!MatchField(0, 8, 0x1abu).matches(bytes.data(), bytes.size()),
          "overwide comparison value does not match by truncation");
  require(!MatchField(0, 0, 0).matches(bytes.data(), bytes.size()), "zero-width field rejected");
  require(!MatchField(0, 33, 0).matches(bytes.data(), bytes.size()), "overwide field rejected");
  require(!MatchField(40, 1, 0).matches(bytes.data(), bytes.size()),
          "offset beyond buffer rejected");
  require(!MatchField(39, 2, 0).matches(bytes.data(), bytes.size()), "partial last byte rejected");
  require(!MatchField(0, 8, 0).matches(nullptr, 10), "null buffer rejected");
  require(MatchField::Wildcard(0, 32).matches(bytes.data(), bytes.size()),
          "wildcard ignores in-range field contents");
  require(!MatchField::Wildcard(40, 8).matches(bytes.data(), bytes.size()),
          "wildcard still requires the declared field to exist");
  require(!MatchField::Wildcard(0, 8).matches(nullptr, 1), "null wildcard rejected");

  std::vector<uint8_t> large(8200, 0xa5);
  large[300] = 0x7b;
  require(MatchField(2400, 8, 0x7b).matches(large.data(), large.size()),
          "byte indexes above 255 do not wrap");
  require(MatchField(65535, 32, 0xd2d2d2d2u).matches(large.data(), large.size()),
          "bit positions beyond uint16 maximum do not wrap");
  requireOutOfRange([] { matchNameHash(4, 0); }, "fifth name hash rejected");
  requireOutOfRange([] { matchNameHashWildcard(std::numeric_limits<size_t>::max()); },
                    "large name hash index rejected before arithmetic");
  requireOutOfRange([] { matchOutPortByte(4, 0); }, "fifth output byte rejected");
}

void testFlowPipeline()
{
  const uint8_t bytes[] = {0xab, 0xcd};
  FlowTable table;
  FlowEntry wildcard;
  wildcard.priority = 1;
  wildcard.matchFields.push_back(MatchField::Wildcard(0, 8));
  wildcard.actions.push_back(FlowAction::Output(1));
  table.addEntry(wildcard);
  FlowEntry exact;
  exact.priority = 10;
  exact.matchFields.push_back(MatchField(0, 8, 0xab));
  exact.actions.push_back(FlowAction::Output(2));
  table.addEntry(exact);
  const auto* selected = table.lookup(bytes, sizeof(bytes));
  require(selected != nullptr && selected->priority == 10, "higher priority wins over wildcard");

  FlowTablePipeline missing;
  require(missing.process(bytes, sizeof(bytes)).dropped, "missing starting table drops");
  require(missing.process(nullptr, 0).dropped, "null empty input drops safely");
  require(missing.process(nullptr, 12).dropped, "null nonempty input drops safely");

  FlowTablePipeline pipeline;
  FlowEntry write;
  write.actions.push_back(FlowAction::SetField(1, 1, {0x42}));
  write.actions.push_back(FlowAction::GotoTable(1));
  pipeline.addTable(0).addEntry(write);
  FlowEntry next;
  next.matchFields.push_back(MatchField(8, 8, 0x42));
  next.actions.push_back(FlowAction::Output(7));
  pipeline.addTable(1).addEntry(next);
  auto result = pipeline.process(bytes, sizeof(bytes));
  require(!result.dropped && result.outputPorts == std::set<uint16_t>({7}),
          "later table sees modified bytes");
  require(result.modifiedHeader == std::vector<uint8_t>({0xab, 0x42}),
          "SET_FIELD offset is measured in bytes");

  const std::vector<FlowAction> invalidWrites = {
      FlowAction::SetField(2, 1, {0}), FlowAction::SetField(1, 2, {0, 0}),
      FlowAction::SetField(0, 2, {0}), FlowAction::SetField(65535, 65535, {0})};
  for (const auto& action : invalidWrites)
  {
    pipeline.getTable(0).clear();
    FlowEntry malformed;
    malformed.actions.push_back(FlowAction::OutputMulticast(9));
    malformed.actions.push_back(action);
    malformed.actions.push_back(FlowAction::Output(7));
    pipeline.getTable(0).addEntry(malformed);
    result = pipeline.process(bytes, sizeof(bytes));
    require(result.dropped && result.outputPorts.empty(), "invalid write drops pending outputs");
    require(result.modifiedHeader == std::vector<uint8_t>({0xab, 0xcd}),
            "invalid write cannot partially modify header");
  }

  pipeline.getTable(0).clear();
  FlowEntry loop;
  loop.actions.push_back(FlowAction::GotoTable(0));
  pipeline.getTable(0).addEntry(loop);
  require(pipeline.process(bytes, sizeof(bytes)).dropped, "table cycle is bounded");
}

void testSwitch()
{
  SwitchPipeline pipeline;
  pipeline.initPipeline();
  const auto query = parse({0x01, 0, 0, 0, 0, 0, 1, 0x10, 0x20, 0x30, 0x40, 0});
  auto result = pipeline.processPacket(query);
  require(result.dropped && result.outputPorts.empty(), "Interest without route drops");
  require(!pipeline.diagnoseFibLookup(query).matched, "table miss is not a FIB match");

  pipeline.installFibEntry({0x10}, 1);
  pipeline.installFibEntry({0x10, 0x20}, 32);
  pipeline.installFibEntry({0x10, 0x20, 0x30, 0x40}, 0xabcd);
  pipeline.installFibEntry({0x10, 0x20, 0x31}, 8);
  result = pipeline.processPacket(query);
  auto diagnostic = pipeline.diagnoseFibLookup(query);
  require(!result.dropped && result.outputPorts == std::set<uint16_t>({0xabcd}),
          "longest represented prefix wins and retains 16-bit output port");
  require(diagnostic.matched && diagnostic.matchLength == 4 && diagnostic.outputPort == 0xabcd,
          "FIB diagnostic agrees with selected action");
  require(result.modifiedHeader[0] == static_cast<uint8_t>(PacketType::UNPROCESSED),
          "FIB action resets processing type");
  pipeline.removeFibEntry({0x10, 0x20, 0x30, 0x40});
  result = pipeline.processPacket(query);
  require(result.outputPorts == std::set<uint16_t>({32}), "removal exposes parent prefix");
  pipeline.installFibEntry({0x10, 0x20, 0xff, 0xff}, 9);
  result = pipeline.processPacket(query);
  require(result.outputPorts == std::set<uint16_t>({9}), "padded prefix replaces equivalent route");
  pipeline.removeFibEntry({0x10, 0x20});
  require(pipeline.processPacket(query).outputPorts == std::set<uint16_t>({1}),
          "short and padded prefix representations remove the same route");

  PacketHeader header;
  require(pipeline.processPacket(header).outputPorts ==
              std::set<uint16_t>({StatimPort::STATEFUL_MODULE}),
          "unprocessed packets reach stateful module");
  header.setType(PacketType::CONTROL);
  require(pipeline.processPacket(header).packetIn, "control packets generate PacketIn");
  header.setType(PacketType::DATA);
  require(pipeline.processPacket(header).dropped, "empty Data output bitmap drops");

  for (uint16_t port = 1; port <= 32; ++port)
  {
    const uint32_t mask = StatefulForwarder::encodeOutPortsMask({port});
    require(mask == (uint32_t{1} << (32 - port)), "port maps to network-order bitmap bit");
    header.setOutPorts(mask);
    result = pipeline.processPacket(header);
    require(!result.dropped && result.outputPorts == std::set<uint16_t>({port}),
            "each bitmap bit selects its one-based output port");
  }
  const std::set<uint16_t> ports = {1, 8, 9, 16, 17, 24, 25, 32};
  header.setOutPorts(StatefulForwarder::encodeOutPortsMask(ports));
  require(pipeline.processPacket(header).outputPorts == ports,
          "multicast crosses all byte boundaries");
  require(StatefulForwarder::encodeOutPortsMask({0, 33, 0xfffd, 0xffff}) == 0,
          "reserved and out-of-range ports do not enter bitmap");
  require(StatefulForwarder::encodeOutPortsMask({0, 1, 32, 33, 0xffff}) == 0x80000001u,
          "invalid ports do not affect valid edge bits");
  header.setOutPorts(0xffffffffu);
  result = pipeline.processPacket(header);
  require(!result.dropped && result.outputPorts.size() == 32, "all 32 output bits are usable");

  const auto bytes = serialize(query);
  for (size_t size = 0; size < 12; ++size)
  {
    require(pipeline.processPacket(bytes.data(), size).dropped, "switch rejects truncated header");
  }
}

void testContentStore()
{
  ContentStore disabled(0);
  ::ndn::Data unowned(::ndn::Name("/disabled"));
  disabled.insert(unowned);
  require(disabled.size() == 0, "zero capacity disables insertion");

  ContentStore store(4);
  bool rejectedUnowned = false;
  try
  {
    store.insert(unowned);
  }
  catch (const std::bad_weak_ptr&)
  {
    rejectedUnowned = true;
  }
  require(rejectedUnowned && store.size() == 0,
          "unowned Data fails without mutating an enabled cache");
  auto original = std::make_shared<::ndn::Data>(::ndn::Name("/entry"));
  auto replacement = std::make_shared<::ndn::Data>(original->getName());
  store.insert(*original);
  store.insert(*replacement);
  require(store.size() == 1 && store.find(original->getName()) == replacement,
          "duplicate name replaces data without growing cache");
  require(store.find(::ndn::Name("/absent")) == nullptr, "cache miss returns null");
  for (unsigned i = 0; i < 200; ++i)
  {
    store.insert(*std::make_shared<::ndn::Data>(::ndn::Name("/data/" + std::to_string(i))));
    require(store.size() <= 4, "every insertion enforces capacity");
  }
  store.setMaxSize(1);
  require(store.size() <= 1, "shrinking immediately enforces capacity");
  store.setMaxSize(0);
  require(store.size() == 0, "shrinking to zero clears cache");
  store.setMaxSize(2);
  store.insert(*original);
  require(store.size() == 1, "cache can be enabled after zero capacity");
  store.clear();
  require(store.size() == 0, "clear removes all cached entries");

  // Choose a seed whose first two coin flips keep both entries, exercising
  // deterministic fallback eviction at capacity one.
  unsigned fallbackSeed = 0;
  for (; fallbackSeed < 10000; ++fallbackSeed)
  {
    std::srand(fallbackSeed);
    const int first = std::rand() % 2;
    const int second = std::rand() % 2;
    if (first == 0 && second == 0)
      break;
  }
  require(fallbackSeed < 10000, "found deterministic no-eviction scan seed");
  std::srand(fallbackSeed);
  ContentStore fallback(1);
  fallback.insert(*std::make_shared<::ndn::Data>(::ndn::Name("/a")));
  fallback.insert(*std::make_shared<::ndn::Data>(::ndn::Name("/b")));
  require(fallback.size() == 1, "fallback eviction enforces capacity when both coins keep");
  require(fallback.find(::ndn::Name("/b")) != nullptr, "fallback evicts in name order");

  // Check that ordinary capacity-100 eviction keeps the original scan's
  // membership and consumes exactly the same sequence of random numbers.
  std::vector<std::shared_ptr<::ndn::Data>> data;
  for (unsigned i = 0; i < 140; ++i)
  {
    data.push_back(std::make_shared<::ndn::Data>(::ndn::Name("/normal/" + std::to_string(i))));
  }
  std::map<::ndn::Name, std::shared_ptr<::ndn::Data>> reference;
  std::srand(12345);
  for (const auto& packet : data)
  {
    reference[packet->getName()] = packet;
    if (reference.size() > 100)
    {
      for (auto it = reference.begin(); it != reference.end();)
      {
        if (std::rand() % 2 == 1)
          it = reference.erase(it);
        else
          ++it;
        if (reference.size() <= 100)
          break;
      }
    }
  }
  require(reference.size() == 100, "ordinary reference scan evicted enough entries");
  const int expectedNextRandom = std::rand();
  std::srand(12345);
  ContentStore normal(100);
  for (const auto& packet : data)
    normal.insert(*packet);
  require(normal.size() == reference.size(), "ordinary scan preserves occupancy");
  for (const auto& packet : data)
  {
    require(static_cast<bool>(normal.find(packet->getName())) ==
                (reference.count(packet->getName()) != 0),
            "ordinary scan preserves cache membership");
  }
  require(std::rand() == expectedNextRandom, "ordinary scan preserves random-number consumption");
}

void countConsumerTransmission(unsigned* observed, std::shared_ptr<const ::ndn::Interest>,
                               ns3::Ptr<ns3::ndn::App>, std::shared_ptr<ns3::ndn::Face>)
{
  ++*observed;
}

void countDeviceTransmission(unsigned* observed, ns3::Ptr<const ns3::Packet>)
{
  ++*observed;
}

template <typename NdnPacket>
ns3::Ptr<ns3::Packet> encodeEnginePacket(const NdnPacket& packet)
{
  auto result = ns3::Create<ns3::Packet>();
  result->AddHeader(ns3::ndn::BlockHeader(packet.wireEncode()));
  result->AddHeader(PacketHeader(packet.getName()));
  return result;
}

void addCounterTestPorts(ForwardingEngine& engine, ns3::NodeContainer& nodes,
                         unsigned* observed = nullptr)
{
  nodes.Create(4);
  ns3::PointToPointHelper links;
  links.SetDeviceAttribute("DataRate", ns3::StringValue("1Gbps"));
  links.SetChannelAttribute("Delay", ns3::StringValue("1ms"));
  for (uint16_t port = 1; port <= 3; ++port)
  {
    auto devices = links.Install(nodes.Get(0), nodes.Get(port));
    require(engine.getPortTable().addDevice(devices.Get(0)) == port,
            "counter fixture uses consecutive real output ports");
    if (observed != nullptr)
    {
      require(devices.Get(0)->TraceConnectWithoutContext(
                  "MacTx", ns3::MakeBoundCallback(&countDeviceTransmission, observed)),
              "counter fixture observes each output device");
    }
  }
}

void checkFibCounterCase(const std::set<uint16_t>& temporaryPorts, uint16_t flowPort,
                          bool enableTemporary, uint64_t expectedTemporary, uint64_t expectedFlow)
{
  ns3::Simulator::Destroy();
  ns3::ndn::StackHelper clockSetup;
  ForwardingEngine engine;
  ns3::NodeContainer nodes;
  unsigned observed = 0;
  addCounterTestPorts(engine, nodes, &observed);
  engine.setEnableTemporaryFib(enableTemporary);
  engine.setEnableInterestReforwarding(false);
  const ::ndn::Name prefix("/counter");
  for (uint16_t port : temporaryPorts)
    engine.getStatefulModule().getTemporaryFib().insert(prefix, port, ::ndn::time::seconds(2));
  if (flowPort != 0)
    engine.getSwitch().installFibEntry(PacketHeader(prefix).getNameHashes(), flowPort);
  ::ndn::Interest interest(::ndn::Name("/counter/item"), ::ndn::time::milliseconds(100));
  interest.setNonce(42);
  engine.onReceiveRawPacket(1, encodeEnginePacket(interest));
  ns3::Simulator::Stop(ns3::MilliSeconds(5));
  ns3::Simulator::Run();
  const auto& counters = engine.getCounters();
  require(counters.temporaryFibInterestTransmissions == expectedTemporary,
          "temporary-FIB count measures output-port sends after ingress filtering");
  require(counters.flowTableFibInterestTransmissions == expectedFlow,
          "flow-table count excludes temporary-FIB output-port sends");
  require(observed == expectedTemporary + expectedFlow,
          "the two disjoint FIB counts equal independently observed device transmissions");
  require(counters.pendingPullsSent == 0, "ordinary FIB forwarding does not count as direct reforwarding");
  ns3::Simulator::Destroy();
}

void testFibTransmissionCounters()
{
  checkFibCounterCase({2}, 2, true, 1, 0);
  checkFibCounterCase({}, 2, true, 0, 1);
  checkFibCounterCase({2, 3}, 2, true, 2, 0);
  checkFibCounterCase({1}, 2, true, 0, 0);
  checkFibCounterCase({}, 0, true, 0, 0);
  checkFibCounterCase({3}, 2, false, 0, 1);
}

void testDirectReforwardingCounterIsolation()
{
  ns3::Simulator::Destroy();
  ns3::ndn::StackHelper clockSetup;
  ForwardingEngine engine;
  ns3::NodeContainer nodes;
  addCounterTestPorts(engine, nodes);
  engine.setEnableInterestReforwarding(true);
  engine.setControllerDelay(ns3::Seconds(10));
  engine.getSwitch().installFibEntry(PacketHeader(::ndn::Name("/rv")).getNameHashes(), 2);
  ::ndn::Interest ordinary(::ndn::Name("/rv/alice/item"), ::ndn::time::milliseconds(100));
  ordinary.setNonce(42);
  engine.onReceiveRawPacket(3, encodeEnginePacket(ordinary));
  ::ndn::Name traceName("/rv/trace/alice");
  traceName.appendSequenceNumber(1);
  ::ndn::Interest trace(traceName, ::ndn::time::milliseconds(100));
  trace.setNonce(43);
  engine.onReceiveRawPacket(1, encodeEnginePacket(trace));
  ns3::Simulator::Stop(ns3::MilliSeconds(5));
  ns3::Simulator::Run();
  require(engine.getCounters().flowTableFibInterestTransmissions == 2,
          "ordinary and trace Interests use the installed route before the update");
  ::ndn::Data data(traceName);
  ::ndn::Signature signature;
  signature.setInfo(::ndn::SignatureInfo(static_cast<::ndn::tlv::SignatureTypeValue>(255)));
  signature.setValue(::ndn::makeNonNegativeIntegerBlock(::ndn::tlv::SignatureValue, 0));
  data.setSignature(signature);
  engine.onReceiveRawPacket(2, encodeEnginePacket(data));
  ns3::Simulator::Stop(ns3::MilliSeconds(5));
  ns3::Simulator::Run();
  const auto& counters = engine.getCounters();
  require(counters.pendingPullsSent == 1, "Trace Data directly resends the one saved ordinary Interest");
  require(counters.temporaryFibInterestTransmissions == 0,
          "direct remembered-Interest send is independent of temporary-FIB sends");
  require(counters.flowTableFibInterestTransmissions == 2,
          "direct remembered-Interest send is independent of flow-table-FIB sends");
  ns3::Simulator::Destroy();
}

void testKiteRouteLeaseFromInterest()
{
  ns3::Simulator::Destroy();
  ns3::ndn::StackHelper clockSetup;
  ForwardingEngine engine;
  ns3::NodeContainer nodes;
  addCounterTestPorts(engine, nodes);
  engine.setEnableInterestReforwarding(false);
  engine.setControllerDelay(ns3::MilliSeconds(20));
  engine.setSyncAckLossRate(1);
  engine.setFlowTableFibEntryLifetime(ns3::MilliSeconds(100));
  engine.setTemporaryFibEntryLifetime(ns3::MilliSeconds(100));
  engine.getSwitch().installFibEntry(PacketHeader(::ndn::Name("/rv")).getNameHashes(), 2);
  ::ndn::Name traceName("/rv/trace/alice");
  traceName.appendSequenceNumber(1);
  ::ndn::Interest trace(traceName, ::ndn::time::milliseconds(100));
  trace.setNonce(51);
  engine.onReceiveRawPacket(1, encodeEnginePacket(trace));
  ns3::Simulator::Stop(ns3::MilliSeconds(5));
  ns3::Simulator::Run();

  // Normal KITE TD content contains no expiration TLV. Its route lifetime
  // comes from the saved TI and begins when this TD creates the route.
  ::ndn::Data data(traceName);
  const std::array<uint8_t, 32> content = {{0}};
  data.setContent(content.data(), content.size());
  ::ndn::Signature signature;
  signature.setInfo(::ndn::SignatureInfo(static_cast<::ndn::tlv::SignatureTypeValue>(255)));
  signature.setValue(::ndn::makeNonNegativeIntegerBlock(::ndn::tlv::SignatureValue, 0));
  data.setSignature(signature);
  engine.onReceiveRawPacket(2, encodeEnginePacket(data));
  // Stop after the FlowMod timestamp: that event is scheduled by the TD
  // handler, after this stop event would otherwise receive its sequence ID.
  ns3::Simulator::Stop(ns3::MilliSeconds(21));
  ns3::Simulator::Run();
  const ::ndn::Name prefix("/rv/alice");
  PacketHeader query(::ndn::Name("/rv/alice/item"));
  require(engine.getStatefulModule().getTemporaryFib().findExact(prefix) != nullptr,
          "plain TD obtains its temporary route lifetime from the saved TI");
  require(engine.getSwitch().diagnoseFibLookup(query).outputPort == 1,
          "flow-table route becomes visible before the common deadline");
  ns3::Simulator::Stop(ns3::MilliSeconds(78));
  ns3::Simulator::Run();
  require(ns3::Simulator::Now() == ns3::MilliSeconds(104), "probe precedes the 105 ms route deadline");
  require(engine.getStatefulModule().getTemporaryFib().findExact(prefix) != nullptr &&
              engine.getSwitch().diagnoseFibLookup(query).outputPort == 1,
          "both route copies remain valid just before the shared deadline");
  ns3::Simulator::Stop(ns3::MilliSeconds(1));
  ns3::Simulator::Run();
  require(engine.getStatefulModule().getTemporaryFib().findExact(prefix) == nullptr,
          "temporary route expires at local TD acceptance plus the TI lifetime");
  require(engine.getSwitch().diagnoseFibLookup(query).outputPort == 2,
          "flow route expires at the same deadline and lookup returns to the static route");
  require(engine.getCentralController().getCounters().flowModsApplied == 1,
          "the delay does not prevent an unexpired installation");
  ns3::Simulator::Destroy();
}

void testRouteLeaseConfigurationValidation()
{
  ns3::Simulator::Destroy();
  ns3::ndn::StackHelper clockSetup;
  ForwardingEngine engine;
  ns3::NodeContainer nodes;
  addCounterTestPorts(engine, nodes);
  engine.setEnableInterestReforwarding(false);
  engine.setFlowTableFibEntryLifetime(ns3::MilliSeconds(100));
  engine.setTemporaryFibEntryLifetime(ns3::MilliSeconds(200));
  engine.getSwitch().installFibEntry(PacketHeader(::ndn::Name("/rv")).getNameHashes(), 2);
  ::ndn::Name traceName("/rv/trace/alice");
  traceName.appendSequenceNumber(1);
  ::ndn::Interest trace(traceName, ::ndn::time::milliseconds(100));
  trace.setNonce(53);
  engine.onReceiveRawPacket(1, encodeEnginePacket(trace));
  ns3::Simulator::Stop(ns3::MilliSeconds(1));
  ns3::Simulator::Run();
  ::ndn::Data data(traceName);
  ::ndn::Signature signature;
  signature.setInfo(::ndn::SignatureInfo(static_cast<::ndn::tlv::SignatureTypeValue>(255)));
  signature.setValue(::ndn::makeNonNegativeIntegerBlock(::ndn::tlv::SignatureValue, 0));
  data.setSignature(signature);
  engine.onReceiveRawPacket(2, encodeEnginePacket(data));
  bool inconsistentSettersRejectedAtUse = false;
  try
  {
    ns3::Simulator::Stop(ns3::MilliSeconds(1));
    ns3::Simulator::Run();
  }
  catch (const std::invalid_argument&)
  {
    inconsistentSettersRejectedAtUse = true;
  }
  require(inconsistentSettersRejectedAtUse,
          "different configured FIB lifetimes are rejected at first route admission");
  require(engine.getStatefulModule().getTemporaryFibSize() == 0,
          "inconsistent engine configuration does not admit a pending update");
  LocalRouteController local;
  local.setEntryLifetime(ns3::MilliSeconds(100));
  const ::ndn::Name prefix("/lease/config");
  const PacketHeader header(prefix);
  bool inconsistentProtocolRejected = false;
  try
  {
    local.applyTraceUpdate(prefix, header.getNameHashes(), prefix.size(), 1, 1,
                           ::ndn::time::milliseconds(50));
  }
  catch (const std::invalid_argument&)
  {
    inconsistentProtocolRejected = true;
  }
  require(inconsistentProtocolRejected, "a supplied lease must agree with the local controller's own configuration");
  require(local.fibSize() == 0, "invalid local lease configuration does not admit a pending update");
  ns3::Simulator::Destroy();
}

void testConsumerCounters()
{
  ns3::Simulator::Destroy();
  ns3::NodeContainer nodes;
  nodes.Create(2);
  ns3::ndn::StackHelper stack;
  stack.Install(nodes);
  unsigned observed[2] = {0, 0};
  ns3::ApplicationContainer applications;
  const char* types[] = {"ns3::ndn::StatimConsumer", "ns3::ndn::KitePullConsumer"};
  for (unsigned i = 0; i < 2; ++i)
  {
    ns3::ndn::AppHelper helper(types[i]);
    helper.SetAttribute("Prefix", ns3::StringValue("/counter-test"));
    helper.SetAttribute("Frequency", ns3::DoubleValue(10));
    helper.SetAttribute("StartSeq", ns3::IntegerValue(70));
    helper.SetAttribute("RetxTimer", ns3::StringValue("10s"));
    if (i == 0)
    {
      helper.SetAttribute("LossWindowStart", ns3::TimeValue(ns3::NanoSeconds(12345678)));
      helper.SetAttribute("LossWindowLen", ns3::TimeValue(ns3::NanoSeconds(234567891)));
    }
    auto application = helper.Install(nodes.Get(i));
    application.Start(ns3::Seconds(0));
    application.Stop(ns3::Seconds(0.25));
    require(application.Get(0)->TraceConnectWithoutContext(
                "TransmittedInterests",
                ns3::MakeBoundCallback(&countConsumerTransmission, &observed[i])),
            "independent transmission observer is attached");
    applications.Add(application);
  }
  ns3::Simulator::Stop(ns3::Seconds(0.3));
  std::ostringstream captured;
  auto* originalBuffer = std::cerr.rdbuf(captured.rdbuf());
  try
  {
    ns3::Simulator::Run();
  }
  catch (...)
  {
    std::cerr.rdbuf(originalBuffer);
    throw;
  }
  std::cerr.rdbuf(originalBuffer);
  require(captured.str().find("window_start=0.012345678s window_len=0.234567891s") != std::string::npos,
          "window timing fields explicitly retain nanosecond decimal precision");
  const auto statim = ns3::DynamicCast<ns3::ndn::StatimConsumer>(applications.Get(0));
  const auto kite = ns3::DynamicCast<ns3::ndn::KitePullConsumer>(applications.Get(1));
  require(observed[0] == 3, "Statim sends at 0, 0.1, and 0.2 seconds before stopping");
  require(observed[1] == 3, "KITE sends at 0, 0.1, and 0.2 seconds before stopping");
  require(statim->GetInterestSent() == observed[0], "Statim does not count its cancelled send");
  require(kite->GetInterestSent() == observed[1], "KITE does not count its cancelled send");
  require(statim->GetUniqueInterestCount() == 3, "Statim unique count is independent of StartSeq");
  require(kite->GetUniqueInterestCount() == 3, "KITE unique count is independent of StartSeq");
  ns3::Simulator::Destroy();
}

} // namespace

int main()
{
  try
  {
    testHeader();
    testTraceEncoding();
    testMatchFields();
    testFlowPipeline();
    testSwitch();
    testContentStore();
    testFibTransmissionCounters();
    testDirectReforwardingCounterIsolation();
    testKiteRouteLeaseFromInterest();
    testRouteLeaseConfigurationValidation();
    testConsumerCounters();
    ns3::Simulator::Destroy();
    std::cout << "packet-pipeline: PASS (" << checks << " checks)\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "packet-pipeline: FAIL after " << checks << " checks: " << error.what() << '\n';
    ns3::Simulator::Destroy();
    return 1;
  }
}
