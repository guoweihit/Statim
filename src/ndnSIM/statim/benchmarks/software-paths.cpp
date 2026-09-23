// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/ndnSIM/statim/control/control-message.hpp"
#include "ns3/ndnSIM/statim/control/local-route-controller.hpp"
#include "ns3/ndnSIM/statim/packet-header.hpp"
#include "ns3/ndnSIM/statim/pipeline/switch-pipeline.hpp"
#include "ns3/ndnSIM/statim/state/temporary-forwarding-table.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace benchmark
{
// Measure temporary-FIB longest-prefix matching and flow-table-FIB lookup.
// The fixtures below define the synthetic NDN name workloads for each path.

using ns3::ndn::statim::ControlMessage;
using ns3::ndn::statim::ControlMessageType;
using ns3::ndn::statim::PacketHeader;
using ns3::ndn::statim::PacketType;
using ns3::ndn::statim::SwitchPipeline;

static volatile uint64_t g_sink = 0;

struct Timing
{
  double meanNs;
  double p50Ns;
  double p95Ns;
  double p99Ns;
  size_t samples;
};

static double percentile(std::vector<double> values, double q)
{
  if (values.empty())
  {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  size_t rank = static_cast<size_t>(std::ceil(q * values.size()));
  rank = std::max<size_t>(1, rank) - 1;
  return values[std::min(rank, values.size() - 1)];
}

template <typename Operation>
static Timing runBatched(uint64_t operations, uint64_t batchSize, Operation operation)
{
  typedef std::chrono::steady_clock Clock;
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>((operations + batchSize - 1) / batchSize));
  double totalNs = 0.0;

  for (uint64_t first = 0; first < operations; first += batchSize)
  {
    const uint64_t count = std::min(batchSize, operations - first);
    const auto begin = Clock::now();
    for (uint64_t j = 0; j < count; ++j)
    {
      operation(first + j);
    }
    const auto end = Clock::now();
    const double elapsed = std::chrono::duration<double, std::nano>(end - begin).count();
    totalNs += elapsed;
    samples.push_back(elapsed / static_cast<double>(count));
  }

  Timing result;
  result.meanNs = totalNs / static_cast<double>(operations);
  result.p50Ns = percentile(samples, 0.50);
  result.p95Ns = percentile(samples, 0.95);
  result.p99Ns = percentile(samples, 0.99);
  result.samples = samples.size();
  return result;
}

static std::vector<std::string> makeDistinctHashTokens(size_t count)
{
  std::set<uint8_t> seen;
  std::vector<std::string> tokens;
  for (uint64_t i = 0; tokens.size() < count && i < 1000000; ++i)
  {
    const std::string token = std::string("h") + std::to_string(i);
    const ::ndn::Name one(std::string("/") + token);
    const uint8_t hash = PacketHeader::computeComponentHash(one.get(0));
    if (seen.insert(hash).second)
    {
      tokens.push_back(token);
    }
  }
  if (tokens.size() != count)
  {
    std::fprintf(stderr, "cannot construct %zu distinct component hashes\n", count);
    std::exit(2);
  }
  return tokens;
}

struct TemporaryFibFixture
{
  ::ns3::ndn::statim::TemporaryForwardingTable fib;
  std::vector<::ndn::Name> hitNames;
  std::vector<::ndn::Name> missNames;

  explicit TemporaryFibFixture(size_t tableSize)
  {
    hitNames.reserve(tableSize);
    missNames.reserve(tableSize);
    for (size_t i = 0; i < tableSize; ++i)
    {
      ::ndn::Name prefix("/benchmark/temporary-fib");
      prefix.appendSequenceNumber(i);
      fib.insert(prefix, static_cast<uint16_t>(1 + (i % 32)), ::ndn::time::hours(24));

      ::ndn::Name hit(prefix);
      hit.append("segment");
      hitNames.push_back(hit);

      ::ndn::Name miss("/benchmark/temporary-fib-miss");
      miss.appendSequenceNumber(i);
      miss.append("segment");
      missNames.push_back(miss);
    }
  }

  bool validate()
  {
    for (size_t i = 0; i < hitNames.size(); ++i)
    {
      if (fib.findLongestPrefix(hitNames[i]) == nullptr ||
          fib.findLongestPrefix(missNames[i]) != nullptr)
      {
        return false;
      }
    }
    return true;
  }
};

struct FlowTableFibFixture
{
  SwitchPipeline sw;
  std::vector<PacketHeader> hitHeaders;
  std::vector<PacketHeader> missHeaders;
  std::vector<uint16_t> expectedPorts;

  FlowTableFibFixture(size_t tableSize, const std::vector<std::string>& tokens)
  {
    sw.initPipeline();
    hitHeaders.reserve(tableSize);
    missHeaders.reserve(tableSize);
    expectedPorts.reserve(tableSize);

    const ::ndn::Name fixed("/flow-table-fib");
    const uint8_t fixedHash = PacketHeader::computeComponentHash(fixed.get(0));
    std::string missComponent;
    for (uint64_t k = 0; k < 100000; ++k)
    {
      const std::string candidate = std::string("miss") + std::to_string(k);
      const ::ndn::Name n(std::string("/") + candidate);
      if (PacketHeader::computeComponentHash(n.get(0)) != fixedHash)
      {
        missComponent = candidate;
        break;
      }
    }
    if (missComponent.empty())
    {
      std::fprintf(stderr, "cannot construct a guaranteed flow-table FIB miss\n");
      std::exit(2);
    }

    for (size_t i = 0; i < tableSize; ++i)
    {
      const size_t a = i / tokens.size();
      const size_t b = i % tokens.size();
      const std::string suffix = tokens.at(a) + "/" + tokens.at(b);
      const ::ndn::Name prefix(std::string("/benchmark/flow-table-fib/") + suffix);
      PacketHeader hit(prefix);
      hit.setType(PacketType::INTEREST);
      const uint16_t port = static_cast<uint16_t>(1 + (i % 32));
      sw.installFibEntry(hit.getNameHashes(), port);
      hitHeaders.push_back(hit);
      expectedPorts.push_back(port);

      const ::ndn::Name miss(std::string("/benchmark/") + missComponent + "/" + suffix);
      PacketHeader missHeader(miss);
      missHeader.setType(PacketType::INTEREST);
      missHeaders.push_back(missHeader);
    }
  }

  bool validate()
  {
    for (size_t i = 0; i < hitHeaders.size(); ++i)
    {
      const auto hit = sw.processPacket(hitHeaders[i]);
      if (hit.outputPorts.count(expectedPorts[i]) != 1)
      {
        return false;
      }
      const auto miss = sw.processPacket(missHeaders[i]);
      if (!miss.dropped || !miss.outputPorts.empty())
      {
        return false;
      }
    }
    return true;
  }
};

struct LookupCell
{
  bool flowTableFib;
  bool hit;
  size_t sizeIndex;
};

static void printHeader()
{
  std::printf("benchmark,path,outcome,table_size,rep,bookkeeping,temporary_fib_write,");
  std::printf("control_dispatch,ops,batch_samples,mean_ns,p50_ns,p95_ns,p99_ns,");
  std::printf("accepted_or_matches,temporary_fib_write_attempts,control_events,valid\n");
}

static bool runLookupSuite(uint64_t operations, uint64_t batchSize, int repetitions)
{
  const std::vector<size_t> sizes = {1, 16, 256, 4096};
  const std::vector<std::string> tokens = makeDistinctHashTokens(64);
  std::vector<std::unique_ptr<TemporaryFibFixture>> temporary_fib;
  std::vector<std::unique_ptr<FlowTableFibFixture>> flowTableFib;

  for (size_t size : sizes)
  {
    temporary_fib.emplace_back(new TemporaryFibFixture(size));
    flowTableFib.emplace_back(new FlowTableFibFixture(size, tokens));
    if (!temporary_fib.back()->validate() || !flowTableFib.back()->validate())
    {
      std::fprintf(stderr, "lookup fixture validation failed at size %zu\n", size);
      return false;
    }
  }

  std::vector<LookupCell> cells;
  for (size_t si = 0; si < sizes.size(); ++si)
  {
    cells.push_back({false, true, si});
    cells.push_back({false, false, si});
    cells.push_back({true, true, si});
    cells.push_back({true, false, si});
  }

  for (int rep = 0; rep < repetitions; ++rep)
  {
    std::vector<size_t> order(cells.size());
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(41000 + rep);
    std::shuffle(order.begin(), order.end(), rng);

    for (size_t cellIndex : order)
    {
      const LookupCell cell = cells[cellIndex];
      const size_t tableSize = sizes[cell.sizeIndex];
      uint64_t matches = 0;
      Timing timing;

      if (!cell.flowTableFib)
      {
        TemporaryFibFixture& fixture = *temporary_fib[cell.sizeIndex];
        const std::vector<::ndn::Name>& queries = cell.hit ? fixture.hitNames : fixture.missNames;
        timing = runBatched(operations, batchSize,
                            [&](uint64_t i)
                            {
                              auto* entry = fixture.fib.findLongestPrefix(queries[i % tableSize]);
                              if (entry != nullptr)
                              {
                                ++matches;
                                g_sink += entry->getPrefix().size();
                              }
                              else
                              {
                                g_sink ^= (i + tableSize);
                              }
                            });
      }
      else
      {
        FlowTableFibFixture& fixture = *flowTableFib[cell.sizeIndex];
        const std::vector<PacketHeader>& queries =
            cell.hit ? fixture.hitHeaders : fixture.missHeaders;
        timing = runBatched(operations, batchSize,
                            [&](uint64_t i)
                            {
                              const auto result = fixture.sw.processPacket(queries[i % tableSize]);
                              if (!result.outputPorts.empty())
                              {
                                ++matches;
                                g_sink += *result.outputPorts.begin();
                              }
                              else
                              {
                                g_sink ^= (i + tableSize);
                              }
                            });
      }

      const uint64_t expectedMatches = cell.hit ? operations : 0;
      const bool valid = matches == expectedMatches;
      std::printf("lookup,%s,%s,%zu,%d,0,0,0,%llu,%zu,%.3f,%.3f,%.3f,%.3f,",
                  cell.flowTableFib ? "FLOW_TABLE_FIB_LOOKUP" : "TEMPORARY_FIB_LONGEST_PREFIX_MATCH", cell.hit ? "hit" : "miss", tableSize,
                  rep, static_cast<unsigned long long>(operations), timing.samples, timing.meanNs,
                  timing.p50Ns, timing.p95Ns, timing.p99Ns);
      std::printf("%llu,0,0,%d\n", static_cast<unsigned long long>(matches), valid ? 1 : 0);
      std::fflush(stdout);
      if (!valid)
      {
        return false;
      }
    }
  }
  return true;
}

struct TdInput
{
  ::ndn::Name prefix;
  std::vector<uint8_t> hashes;
};

struct TdResult
{
  Timing timing;
  uint64_t accepted;
  uint64_t fibAttempts;
  uint64_t controlEvents;
  bool valid;
};

static TdResult runTdCell(bool bookkeeping, bool temporaryFibWrite, bool controlDispatch,
                          uint64_t operations, uint64_t batchSize,
                          const std::vector<TdInput>& inputs)
{
  std::map<::ndn::Name, uint64_t> seqMap;
  ::ns3::ndn::statim::TemporaryForwardingTable fib;
  const size_t poolSize = inputs.size();
  const ::ndn::time::microseconds ttl = ::ndn::time::hours(24);

  // Establish the same steady-state key set before timing every factorial
  // cell. Subsequent updates use newer sequence numbers and replacement ports.
  for (size_t i = 0; i < poolSize; ++i)
  {
    if (bookkeeping)
    {
      seqMap[inputs[i].prefix] = 1;
    }
    if (temporaryFibWrite)
    {
      fib.insert(inputs[i].prefix, 1, ttl);
    }
  }

  uint64_t accepted = 0;
  uint64_t fibAttempts = 0;
  uint64_t controlEvents = 0;
  std::function<void(const ControlMessage&)> dispatch = [&](const ControlMessage& message)
  {
    ++controlEvents;
    g_sink ^= message.seq + message.prefixLen + message.outputPort + message.nameHashes.size();
  };

  Timing timing = runBatched(operations, batchSize,
                             [&](uint64_t i)
                             {
                               const size_t index = static_cast<size_t>(i % poolSize);
                               const uint64_t cycle = i / poolSize + 2;
                               const uint64_t seq = cycle;
                               const uint16_t port = static_cast<uint16_t>(1 + (cycle % 31));
                               const TdInput& input = inputs[index];
                               bool updateAccepted = true;

                               if (bookkeeping)
                               {
                                 auto it = seqMap.find(input.prefix);
                                 if (it != seqMap.end() && seq <= it->second)
                                 {
                                   updateAccepted = false;
                                 }
                                 else
                                 {
                                   seqMap[input.prefix] = seq;
                                 }
                               }
                               if (updateAccepted)
                               {
                                 ++accepted;
                                 if (temporaryFibWrite)
                                 {
                                   ++fibAttempts;
                                   // Measure the same erase-and-insert generation replacement used
                                   // by the local controller.
                                   fib.erase(input.prefix);
                                   fib.insert(input.prefix, port, ttl);
                                 }
                                 if (controlDispatch)
                                 {
                                   ControlMessage message(ControlMessageType::PACKET_IN,
                                                          input.hashes, input.prefix.size(), port,
                                                          input.prefix, seq, ns3::Seconds(0));
                                   dispatch(message);
                                 }
                               }
                               g_sink ^= seq + index;
                             });

  const bool valid = accepted == operations && fibAttempts == (temporaryFibWrite ? operations : 0) &&
                     controlEvents == (controlDispatch ? operations : 0);
  return {timing, accepted, fibAttempts, controlEvents, valid};
}

static bool validateProductionDedupGate()
{
  ns3::ndn::statim::LocalRouteController tc;
  uint64_t dispatched = 0;
  tc.setFlowTableSyncRequestCallback([&](const ControlMessage&) { ++dispatched; });
  const ::ndn::Name prefix("/benchmark/duplicate-rejection/check");
  PacketHeader header(prefix);
  const bool first = tc.applyTraceUpdate(prefix, header.getNameHashes(), prefix.size(), 2, 7,
                                         ::ndn::time::hours(1));
  const bool duplicate = tc.applyTraceUpdate(prefix, header.getNameHashes(), prefix.size(), 2, 7,
                                             ::ndn::time::hours(1));
  return first && !duplicate && dispatched == 1 && tc.fibSize() == 1 &&
         tc.getCounters().tempUpdates == 1 && tc.getCounters().controlPackets == 1;
}

static bool runTdSuite(uint64_t operations, uint64_t batchSize, int repetitions)
{
  static const size_t POOL_SIZE = 4096;
  std::vector<TdInput> inputs;
  inputs.reserve(POOL_SIZE);
  for (size_t i = 0; i < POOL_SIZE; ++i)
  {
    ::ndn::Name prefix("/benchmark/trace-update");
    prefix.appendSequenceNumber(i);
    PacketHeader header(prefix);
    inputs.push_back({prefix, header.getNameHashes()});
  }

  if (!validateProductionDedupGate())
  {
    std::fprintf(stderr, "production dedup-gate validation failed\n");
    return false;
  }
  std::printf("# validation,production_dedup_gate,pass\n");

  for (int rep = 0; rep < repetitions; ++rep)
  {
    std::vector<int> order = {0, 1, 2, 3, 4, 5, 6, 7};
    std::mt19937 rng(42000 + rep);
    std::shuffle(order.begin(), order.end(), rng);
    for (int mask : order)
    {
      const bool bookkeeping = (mask & 1) != 0;
      const bool temporaryFibWrite = (mask & 2) != 0;
      const bool controlDispatch = (mask & 4) != 0;
      TdResult result =
          runTdCell(bookkeeping, temporaryFibWrite, controlDispatch, operations, batchSize, inputs);
      std::printf("trace_update,SOFTWARE_ACCEPTED,factorial,4096,%d,%d,%d,%d,", rep,
                  bookkeeping ? 1 : 0, temporaryFibWrite ? 1 : 0, controlDispatch ? 1 : 0);
      std::printf("%llu,%zu,%.3f,%.3f,%.3f,%.3f,", static_cast<unsigned long long>(operations),
                  result.timing.samples, result.timing.meanNs, result.timing.p50Ns,
                  result.timing.p95Ns, result.timing.p99Ns);
      std::printf("%llu,%llu,%llu,%d\n", static_cast<unsigned long long>(result.accepted),
                  static_cast<unsigned long long>(result.fibAttempts),
                  static_cast<unsigned long long>(result.controlEvents), result.valid ? 1 : 0);
      std::fflush(stdout);
      if (!result.valid)
      {
        return false;
      }
    }
  }
  return true;
}

} // namespace benchmark

int main(int argc, char* argv[])
{
  uint64_t lookupOperations = 10000;
  uint64_t tdOperations = 65536;
  int repetitions = 10;
  uint64_t batchSize = 128;
  if (argc > 1)
    lookupOperations = std::strtoull(argv[1], nullptr, 10);
  if (argc > 2)
    tdOperations = std::strtoull(argv[2], nullptr, 10);
  if (argc > 3)
    repetitions = std::atoi(argv[3]);
  if (argc > 4)
    batchSize = std::strtoull(argv[4], nullptr, 10);
  if (lookupOperations == 0 || tdOperations == 0 || repetitions <= 0 || batchSize == 0)
  {
    std::fprintf(stderr, "usage: %s [lookup_operations [td_operations [repetitions [batch]]]]\n",
                 argv[0]);
    return 2;
  }

  std::printf("# Statim software-path microbenchmark v1\n");
  std::printf("# lookup_operations=%llu td_operations=%llu repetitions=%d batch=%llu\n",
              static_cast<unsigned long long>(lookupOperations),
              static_cast<unsigned long long>(tdOperations), repetitions,
              static_cast<unsigned long long>(batchSize));
  std::printf("# FLOW_TABLE_FIB_LOOKUP measures software STATIM-header serialization ");
  std::printf("and flow-table processing in tables 50/51.\n");
  std::printf(
      "# Trace-update factors use precomputed Name/hash inputs and the accepted-update path.\n");
  benchmark::printHeader();

  if (!benchmark::runLookupSuite(lookupOperations, batchSize, repetitions))
  {
    return 1;
  }
  if (!benchmark::runTdSuite(tdOperations, batchSize, repetitions))
  {
    return 1;
  }
  std::printf("# sink=%llu\n", static_cast<unsigned long long>(benchmark::g_sink));
  return 0;
}
