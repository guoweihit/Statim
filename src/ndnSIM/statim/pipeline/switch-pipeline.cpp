// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/ndnSIM/statim/pipeline/switch-pipeline.hpp"

#include <array>

namespace
{

std::array<uint8_t, ns3::ndn::statim::PacketHeader::SERIALIZED_SIZE>
serializeHeader(const ns3::ndn::statim::PacketHeader& header)
{
  ns3::Buffer buffer;
  buffer.AddAtStart(header.GetSerializedSize());
  header.Serialize(buffer.Begin());
  std::array<uint8_t, ns3::ndn::statim::PacketHeader::SERIALIZED_SIZE> bytes;
  buffer.Begin().Read(bytes.data(), static_cast<uint32_t>(bytes.size()));
  return bytes;
}

} // namespace

namespace ns3
{
namespace ndn
{
namespace statim
{

SwitchPipeline::SwitchPipeline() {}

void SwitchPipeline::initPipeline()
{
  m_pipeline.addTable(StatimTableId::NDN_STATE, "NDNStateTable");
  m_pipeline.addTable(StatimTableId::NDN_FIB, "NDNFibTable");
  m_pipeline.addTable(StatimTableId::MULTI_BYTE1, "MultiByte1Table");
  m_pipeline.addTable(StatimTableId::MULTI_BYTE2, "MultiByte2Table");
  m_pipeline.addTable(StatimTableId::MULTI_BYTE3, "MultiByte3Table");
  m_pipeline.addTable(StatimTableId::MULTI_BYTE4, "MultiByte4Table");

  FlowTable& stateTable = m_pipeline.getTable(StatimTableId::NDN_STATE);

  // --- Interest + out_port == 0 → goto FIB (priority 4500) ---
  {
    FlowEntry e;
    e.matchFields.push_back(matchType(static_cast<uint8_t>(PacketType::INTEREST)));
    e.matchFields.push_back(matchOutPortsZero());
    e.priority = 4500;
    e.actions.push_back(FlowAction::GotoTable(StatimTableId::NDN_FIB));
    stateTable.addEntry(e);
  }

  // --- Unprocessed + out_port wildcard → output to stateful module (priority 4000) ---
  {
    FlowEntry e;
    e.matchFields.push_back(matchType(static_cast<uint8_t>(PacketType::UNPROCESSED)));
    e.matchFields.push_back(matchOutPortsWildcard());
    e.priority = 4000;
    e.actions.push_back(FlowAction::Output(StatimPort::STATEFUL_MODULE));
    stateTable.addEntry(e);
  }

  // --- Interest + out_port wildcard → goto MultiByte1 (priority 4000) ---
  {
    FlowEntry e;
    e.matchFields.push_back(matchType(static_cast<uint8_t>(PacketType::INTEREST)));
    e.matchFields.push_back(matchOutPortsWildcard());
    e.priority = 4000;
    e.actions.push_back(FlowAction::GotoTable(StatimTableId::MULTI_BYTE1));
    stateTable.addEntry(e);
  }

  // --- Data + out_port wildcard → goto MultiByte1 (priority 4000) ---
  {
    FlowEntry e;
    e.matchFields.push_back(matchType(static_cast<uint8_t>(PacketType::DATA)));
    e.matchFields.push_back(matchOutPortsWildcard());
    e.priority = 4000;
    e.actions.push_back(FlowAction::GotoTable(StatimTableId::MULTI_BYTE1));
    stateTable.addEntry(e);
  }

  // --- Control + out_port wildcard → PacketIn (priority 3500) ---
  {
    FlowEntry e;
    e.matchFields.push_back(matchType(static_cast<uint8_t>(PacketType::CONTROL)));
    e.matchFields.push_back(matchOutPortsWildcard());
    e.priority = 3500;
    e.actions.push_back(FlowAction::PacketIn());
    stateTable.addEntry(e);
  }

  // --- Self-Learning Discovery + out_port wildcard → goto FIB (priority 3000) ---
  {
    FlowEntry e;
    e.matchFields.push_back(matchType(static_cast<uint8_t>(PacketType::SELF_LEARNING_DISCOVERY)));
    e.matchFields.push_back(matchOutPortsWildcard());
    e.priority = 3000;
    e.actions.push_back(FlowAction::GotoTable(StatimTableId::NDN_FIB));
    stateTable.addEntry(e);
  }

  // --- NDN FIB table: default drop (priority 0) ---
  {
    FlowTable& fib = m_pipeline.getTable(StatimTableId::NDN_FIB);
    FlowEntry eDrop;
    eDrop.priority = 0;
    eDrop.actions.push_back(FlowAction::Drop());
    fib.addEntry(eDrop);
  }

  // --- Multicast Byte Tables (52-55) ---
  initMulticastByteTable(StatimTableId::MULTI_BYTE1, 0, StatimTableId::MULTI_BYTE2, false);
  initMulticastByteTable(StatimTableId::MULTI_BYTE2, 1, StatimTableId::MULTI_BYTE3, false);
  initMulticastByteTable(StatimTableId::MULTI_BYTE3, 2, StatimTableId::MULTI_BYTE4, false);
  initMulticastByteTable(StatimTableId::MULTI_BYTE4, 3, 0, true);
}

void SwitchPipeline::initMulticastByteTable(uint8_t tableId, int byteIndex, uint8_t nextTableId,
                                            bool isLast)
{
  FlowTable& table = m_pipeline.getTable(tableId);

  for (int byteVal = 0; byteVal <= 0xFF; ++byteVal)
  {
    FlowEntry entry;
    entry.matchFields.push_back(matchOutPortByte(byteIndex, byteVal));
    entry.priority = 3000;

    entry.actions.push_back(FlowAction::SetField(0, 1, {0x00}));

    for (int i = 1; i <= 8; ++i)
    {
      if (((byteVal >> (8 - i)) & 0x01) == 1)
      {
        uint16_t port = static_cast<uint16_t>(byteIndex * 8 + i);
        entry.actions.push_back(FlowAction::OutputMulticast(port));
      }
    }

    if (!isLast)
    {
      entry.actions.push_back(FlowAction::GotoTable(nextTableId));
    }

    table.addEntry(entry);
  }
}

PipelineResult SwitchPipeline::processPacket(const uint8_t* headerBuf, size_t bufLen) const
{
  ++m_counters.pipelineProcessed;
  if (headerBuf == nullptr || bufLen < PacketHeader::SERIALIZED_SIZE)
  {
    PipelineResult result;
    result.dropped = true;
    return result;
  }
  return m_pipeline.process(headerBuf, bufLen, StatimTableId::NDN_STATE);
}

PipelineResult SwitchPipeline::processPacket(const PacketHeader& header) const
{
  const auto bytes = serializeHeader(header);
  return processPacket(bytes.data(), bytes.size());
}

// Each non-wildcard component adds FIB_PRIORITY_STEP to the match priority.
uint32_t SwitchPipeline::fibPriority(const std::vector<uint8_t>& nameHashes)
{
  uint32_t validCount = 0;
  for (size_t i = 0; i < STATIM_NAME_HASH_COUNT && i < nameHashes.size(); ++i)
  {
    if (nameHashes[i] != 0xFF)
      ++validCount;
  }
  return FIB_PRIORITY_BASE + validCount * FIB_PRIORITY_STEP;
}

void SwitchPipeline::installFibEntry(const std::vector<uint8_t>& nameHashes, uint16_t outputPort,
                                     Time expiry)
{
  ++m_counters.flowModsApplied;

  FlowEntry entry;
  entry.priority = fibPriority(nameHashes);

  entry.matchFields.push_back(matchTypeWildcard());

  for (size_t i = 0; i < STATIM_NAME_HASH_COUNT; ++i)
  {
    if (i < nameHashes.size() && nameHashes[i] != 0xFF)
    {
      entry.matchFields.push_back(matchNameHash(i, nameHashes[i]));
    }
    else
    {
      entry.matchFields.push_back(matchNameHashWildcard(i));
    }
  }

  entry.actions.push_back(FlowAction::SetField(0, 1, {0x00}));
  entry.actions.push_back(FlowAction::OutputMulticast(outputPort));

  if (expiry > Seconds(0))
  {
    entry.hasExpiry = true;
    entry.expiry = expiry;
  }

  removeFibEntry(nameHashes);

  m_pipeline.getTable(StatimTableId::NDN_FIB).addEntry(entry);
}

void SwitchPipeline::removeFibEntry(const std::vector<uint8_t>& nameHashes)
{
  uint32_t prio = fibPriority(nameHashes);

  std::vector<MatchField> expected;
  expected.push_back(matchTypeWildcard());
  for (size_t i = 0; i < STATIM_NAME_HASH_COUNT; ++i)
  {
    if (i < nameHashes.size() && nameHashes[i] != 0xFF)
    {
      expected.push_back(matchNameHash(i, nameHashes[i]));
    }
    else
    {
      expected.push_back(matchNameHashWildcard(i));
    }
  }

  m_pipeline.getTable(StatimTableId::NDN_FIB)
      .removeIf(
          [&](const FlowEntry& e)
          {
            if (e.priority != prio)
              return false;
            if (e.matchFields.size() != expected.size())
              return false;
            for (size_t i = 0; i < expected.size(); ++i)
            {
              if (e.matchFields[i].wildcard != expected[i].wildcard)
                return false;
              if (!e.matchFields[i].wildcard && e.matchFields[i].value != expected[i].value)
                return false;
            }
            return true;
          });
}

SwitchPipeline::FibLookupDiagnostic
SwitchPipeline::diagnoseFibLookup(const PacketHeader& header) const
{
  FibLookupDiagnostic result;
  const auto bytes = serializeHeader(header);
  const FlowEntry* entry =
      m_pipeline.getTable(StatimTableId::NDN_FIB).lookup(bytes.data(), bytes.size());
  if (entry == nullptr)
  {
    return result;
  }

  // Report selected entries with an output action, and derive the represented
  // prefix length from their exact name-hash match fields.
  for (const auto& action : entry->actions)
  {
    if (action.type == FlowActionType::OUTPUT || action.type == FlowActionType::OUTPUT_MULTICAST)
    {
      result.matched = true;
      result.outputPort = action.portNumber;
      break;
    }
  }
  if (!result.matched)
  {
    return result;
  }
  for (size_t i = 1; i < entry->matchFields.size(); ++i)
  {
    if (!entry->matchFields[i].wildcard)
    {
      ++result.matchLength;
    }
  }
  return result;
}

} // namespace statim
} // namespace ndn
} // namespace ns3
