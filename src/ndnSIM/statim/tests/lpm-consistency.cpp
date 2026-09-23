// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/core-module.h"
#include "ns3/ndnSIM/statim/control/control-message.hpp"
#include "ns3/ndnSIM/statim/stateful/stateful-forwarder.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{

using ns3::ndn::statim::ControlMessage;
using ns3::ndn::statim::ControlMessageType;
using ns3::ndn::statim::PacketHeader;
using ns3::ndn::statim::StatefulForwarder;
using ns3::ndn::statim::SwitchPipeline;

struct TableMatch
{
  bool matched;
  size_t length;
  uint16_t port;

  TableMatch() : matched(false), length(0), port(0) {}
};

struct Selection
{
  // Store the selected forwarding table name alongside its output port.
  std::string table;
  uint16_t port;

  Selection(const std::string& tableName = "NONE", uint16_t outputPort = 0)
      : table(tableName), port(outputPort)
  {
  }
};

struct Fixture
{
  StatefulForwarder module;
  uint64_t nextSequence;

  Fixture() : nextSequence(1) {}

  void installFlowTableRoute(const ::ndn::Name& prefix, uint16_t port)
  {
    PacketHeader h(prefix);
    module.getSwitch().installFibEntry(h.getNameHashes(), port);
  }

  uint64_t installTemporaryRoute(const ::ndn::Name& prefix, uint16_t port)
  {
    const uint64_t seq = nextSequence++;
    const bool accepted = module.applyTraceUpdate(prefix, port, seq);
    if (!accepted)
    {
      throw std::runtime_error("temporary FIB update unexpectedly rejected");
    }
    return seq;
  }

  bool complete(const ::ndn::Name& prefix, uint64_t seq)
  {
    PacketHeader h(prefix);
    ControlMessage ack(ControlMessageType::SYNC_ACK, h.getNameHashes(), prefix.size(), 0, prefix,
                       seq);
    return module.getTemporaryController().handleControlResponse(ack);
  }

  TableMatch lookupTemporaryFib(const ::ndn::Name& name)
  {
    TableMatch result;
    auto* entry = module.getTemporaryFib().findLongestPrefix(name);
    if (entry == nullptr)
    {
      return result;
    }
    const auto& hops = entry->getNextHops();
    if (hops.empty())
    {
      return result;
    }
    result.matched = true;
    result.length = entry->getPrefix().size();
    result.port = hops.begin()->second.port;
    return result;
  }

  TableMatch lookupFlowTableFib(const ::ndn::Name& name) const
  {
    TableMatch result;
    PacketHeader h(name);
    h.setType(ns3::ndn::statim::PacketType::INTEREST);
    h.setOutPorts(0);
    const SwitchPipeline::FibLookupDiagnostic d = module.getSwitch().diagnoseFibLookup(h);
    result.matched = d.matched;
    result.length = d.matchLength;
    result.port = d.outputPort;
    return result;
  }
};

std::string showLength(const TableMatch& match)
{
  return match.matched ? std::to_string(match.length) : "-";
}

std::string showPort(const TableMatch& match)
{
  return match.matched ? std::to_string(match.port) : "-";
}

Selection selectSequential(const TableMatch& temp, const TableMatch& flow)
{
  // Match ForwardingEngine::processInterestAfterDelay with temporary forwarding
  // enabled: a temporary-FIB match determines the output port; a miss falls
  // back to the flow-table FIB.
  if (temp.matched)
  {
    return Selection("TEMPORARY_FIB", temp.port);
  }
  if (flow.matched)
  {
    return Selection("FLOW_TABLE_FIB", flow.port);
  }
  return Selection();
}

Selection selectGlobalLpm(const TableMatch& temp, const TableMatch& flow)
{
  if (!temp.matched)
  {
    return flow.matched ? Selection("FLOW_TABLE_FIB", flow.port) : Selection();
  }
  if (!flow.matched)
  {
    return Selection("TEMPORARY_FIB", temp.port);
  }
  if (temp.length >= flow.length)
  {
    // Equal-length temporary state is the newer route in this mechanism.
    return Selection("TEMPORARY_FIB", temp.port);
  }
  return Selection("FLOW_TABLE_FIB", flow.port);
}

bool report(const std::string& caseName, const ::ndn::Name& query, const TableMatch& temp,
            const TableMatch& flow, const Selection& expected, const std::string& note,
            bool completionApplied = false)
{
  const Selection actual = selectSequential(temp, flow);
  const bool pass = actual.table == expected.table && actual.port == expected.port;
  std::cout << caseName << '\t' << query << '\t' << showLength(temp) << '\t' << showPort(temp)
            << '\t' << showLength(flow) << '\t' << showPort(flow) << '\t' << expected.table << '\t'
            << expected.port << '\t' << actual.table << '\t' << actual.port << '\t'
            << (pass ? "true" : "false") << '\t' << (completionApplied ? "true" : "false") << '\t'
            << note << std::endl;
  return pass;
}

} // namespace

int main(int argc, char** argv)
{
  ns3::CommandLine cmd;
  cmd.Parse(argc, argv);

  std::cout << "case\tquery\ttemporary_fib_match_length\ttemporary_fib_port\tflow_table_fib_match_length\tflow_table_fib_port\t"
            << "expected_table\texpected_port\tactual_table\tactual_port\tpass\t"
            << "completion_applied\tnote" << std::endl;

  unsigned expectedPasses = 0;
  unsigned observedPasses = 0;
  unsigned expectedCounterexamples = 0;
  unsigned observedCounterexamples = 0;

  {
    Fixture f;
    f.installFlowTableRoute("/alpha", 1);
    const ::ndn::Name query("/alpha/item/segment");
    const TableMatch temporaryMatch = f.lookupTemporaryFib(query);
    const TableMatch flowTableMatch = f.lookupFlowTableFib(query);
    ++expectedPasses;
    if (report("flow-table-fib-parent-fallback", query, temporaryMatch, flowTableMatch, selectGlobalLpm(temporaryMatch, flowTableMatch),
               "a temporary-FIB miss falls back to the flow-table FIB parent"))
    {
      ++observedPasses;
    }
  }

  {
    Fixture f;
    f.installFlowTableRoute("/alpha", 1);
    f.installTemporaryRoute("/alpha/item", 2);
    const ::ndn::Name query("/alpha/item/segment");
    const TableMatch temporaryMatch = f.lookupTemporaryFib(query);
    const TableMatch flowTableMatch = f.lookupFlowTableFib(query);
    ++expectedPasses;
    if (report("temporary-fib-child-flow-table-fib-parent", query, temporaryMatch, flowTableMatch, selectGlobalLpm(temporaryMatch, flowTableMatch),
               "the temporary FIB contains the globally longer prefix"))
    {
      ++observedPasses;
    }
  }

  {
    Fixture f;
    f.installTemporaryRoute("/alpha", 3);
    f.installFlowTableRoute("/alpha/item", 4);
    const ::ndn::Name query("/alpha/item/segment");
    const TableMatch temporaryMatch = f.lookupTemporaryFib(query);
    const TableMatch flowTableMatch = f.lookupFlowTableFib(query);
    ++expectedCounterexamples;
    if (!report(
            "temporary-fib-parent-flow-table-fib-child-counterexample", query, temporaryMatch, flowTableMatch, selectGlobalLpm(temporaryMatch, flowTableMatch),
            "sequential temporary-FIB-first lookup selects the shorter temporary prefix"))
    {
      ++observedCounterexamples;
    }
  }

  {
    Fixture f;
    f.installFlowTableRoute("/alpha/item", 5);
    f.installTemporaryRoute("/alpha/item", 6);
    const ::ndn::Name query("/alpha/item/segment");
    const TableMatch temporaryMatch = f.lookupTemporaryFib(query);
    const TableMatch flowTableMatch = f.lookupFlowTableFib(query);
    ++expectedPasses;
    if (report("same-prefix-temporary-fib-new-route", query, temporaryMatch, flowTableMatch, selectGlobalLpm(temporaryMatch, flowTableMatch),
               "equal-prefix temporary route is the newer route"))
    {
      ++observedPasses;
    }
  }

  {
    Fixture f;
    f.installTemporaryRoute("/alpha/item", 7);
    f.installFlowTableRoute("/alpha/other", 8);
    const ::ndn::Name query("/alpha/other/segment");
    const TableMatch temporaryMatch = f.lookupTemporaryFib(query);
    const TableMatch flowTableMatch = f.lookupFlowTableFib(query);
    ++expectedPasses;
    if (report("sibling", query, temporaryMatch, flowTableMatch, selectGlobalLpm(temporaryMatch, flowTableMatch),
               "a sibling temporary prefix leaves lookup to the matching flow-table prefix"))
    {
      ++observedPasses;
    }
  }

  {
    Fixture f;
    const ::ndn::Name prefix("/mobile/item");
    const uint64_t seq = f.installTemporaryRoute(prefix, 9);
    f.installFlowTableRoute(prefix, 10);
    const bool completed = f.complete(prefix, seq);
    const ::ndn::Name query("/mobile/item/segment");
    const TableMatch temporaryMatch = f.lookupTemporaryFib(query);
    const TableMatch flowTableMatch = f.lookupFlowTableFib(query);
    ++expectedPasses;
    if (report("completion-fallback", query, temporaryMatch, flowTableMatch, selectGlobalLpm(temporaryMatch, flowTableMatch),
               "SYNC_ACK removes temporary state; lookup uses the installed flow-table route",
               completed))
    {
      ++observedPasses;
    }
  }

  std::cout << "SUMMARY expected_passes=" << expectedPasses << " observed_passes=" << observedPasses
            << " expected_counterexamples=" << expectedCounterexamples
            << " observed_counterexamples=" << observedCounterexamples << " mechanism_changed=false"
            << std::endl;

  ns3::Simulator::Destroy();
  return (observedPasses == expectedPasses && observedCounterexamples == expectedCounterexamples)
             ? 0
             : 1;
}
