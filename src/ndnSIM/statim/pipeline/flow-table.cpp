// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/ndnSIM/statim/pipeline/flow-table.hpp"
#include <algorithm>

namespace ns3
{
namespace ndn
{
namespace statim
{

FlowTable::FlowTable(uint8_t tableId, const std::string& name) : m_tableId(tableId), m_name(name) {}

void FlowTable::addEntry(const FlowEntry& entry)
{
  m_entries.push_back(entry);
  sortEntries();
}

void FlowTable::clear()
{
  m_entries.clear();
}

const FlowEntry* FlowTable::lookup(const uint8_t* headerBuf, size_t bufLen) const
{
  for (const auto& entry : m_entries)
  {
    if (entry.matches(headerBuf, bufLen))
    {
      return &entry;
    }
  }
  return nullptr;
}

void FlowTable::sortEntries()
{
  std::sort(m_entries.begin(), m_entries.end(),
            [](const FlowEntry& a, const FlowEntry& b) { return a.priority > b.priority; });
}

FlowTablePipeline::FlowTablePipeline() {}

FlowTable& FlowTablePipeline::addTable(uint8_t tableId, const std::string& name)
{
  for (auto& t : m_tables)
  {
    if (t.getTableId() == tableId)
    {
      return t;
    }
  }
  m_tables.emplace_back(tableId, name);
  std::sort(m_tables.begin(), m_tables.end(),
            [](const FlowTable& a, const FlowTable& b) { return a.getTableId() < b.getTableId(); });
  return getTable(tableId);
}

FlowTable& FlowTablePipeline::getTable(uint8_t tableId)
{
  for (auto& t : m_tables)
  {
    if (t.getTableId() == tableId)
    {
      return t;
    }
  }
  return addTable(tableId);
}

const FlowTable& FlowTablePipeline::getTable(uint8_t tableId) const
{
  for (const auto& t : m_tables)
  {
    if (t.getTableId() == tableId)
    {
      return t;
    }
  }
  // This is a programming error: callers may only inspect initialized tables.
  throw std::out_of_range("flow table does not exist");
}

PipelineResult FlowTablePipeline::process(const uint8_t* headerBuf, size_t bufLen,
                                          uint8_t startTableId) const
{
  PipelineResult result;
  if (headerBuf == nullptr || bufLen == 0)
  {
    result.dropped = true;
    return result;
  }
  result.modifiedHeader.assign(headerBuf, headerBuf + bufLen);

  uint8_t currentTableId = startTableId;
  static const int MAX_HOPS = 32;

  for (int hop = 0; hop < MAX_HOPS; ++hop)
  {
    const FlowTable* table = nullptr;
    for (const auto& t : m_tables)
    {
      if (t.getTableId() == currentTableId)
      {
        table = &t;
        break;
      }
    }
    if (table == nullptr)
    {
      result.dropped = true;
      return result;
    }

    const FlowEntry* entry =
        table->lookup(result.modifiedHeader.data(), result.modifiedHeader.size());
    if (entry == nullptr)
    {
      result.dropped = true;
      return result;
    }

    result.matched = true;

    bool shouldContinue = false;
    for (const auto& action : entry->actions)
    {
      switch (action.type)
      {

      case FlowActionType::OUTPUT:
        result.outputPorts.insert(action.portNumber);
        result.dropped = false;
        return result;

      case FlowActionType::OUTPUT_MULTICAST:
        result.outputPorts.insert(action.portNumber);
        break;

      case FlowActionType::GOTO_TABLE:
        currentTableId = action.gotoTableId;
        shouldContinue = true;
        break;

      case FlowActionType::PACKET_IN:
        result.packetIn = true;
        return result;

      case FlowActionType::SET_FIELD:
      {
        const size_t off = action.setFieldOffset;
        const size_t len = action.setFieldLength;
        if (off > result.modifiedHeader.size() || len > result.modifiedHeader.size() - off ||
            len > action.setFieldValue.size())
        {
          result.dropped = true;
          result.outputPorts.clear();
          return result;
        }
        std::copy_n(action.setFieldValue.begin(), len, result.modifiedHeader.begin() + off);
        break;
      }

      case FlowActionType::DROP:
        result.dropped = true;
        return result;
      }
    }

    if (!shouldContinue)
    {
      if (!result.outputPorts.empty())
      {
        return result;
      }
      result.dropped = true;
      return result;
    }
  }

  result.dropped = true;
  return result;
}

} // namespace statim
} // namespace ndn
} // namespace ns3
