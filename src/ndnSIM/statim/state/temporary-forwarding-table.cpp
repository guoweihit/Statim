// SPDX-License-Identifier: GPL-3.0-or-later
// See the repository license and its additional permission for linking with ns-3.

#include "temporary-forwarding-table.hpp"

#include <stdexcept>
#include <utility>

namespace ns3
{
namespace ndn
{
namespace statim
{

TemporaryForwardingTable::TemporaryForwardingTable(ClockFunction clock) : m_clock(std::move(clock))
{
  if (!m_clock)
  {
    throw std::invalid_argument("TemporaryForwardingTable requires a clock function");
  }
}

TemporaryForwardingTable::InsertResult
TemporaryForwardingTable::insert(const ::ndn::Name& prefix, Port port, Lifetime lifetime)
{
  return insertUntil(prefix, port, m_clock() + lifetime);
}

TemporaryForwardingTable::InsertResult
TemporaryForwardingTable::insertUntil(const ::ndn::Name& prefix, Port port, TimePoint deadline)
{
  const TimePoint now = m_clock();
  auto found = m_entries.find(prefix);
  const bool prefixCreated = found == m_entries.end();
  if (prefixCreated)
  {
    found = m_entries.emplace(prefix, ForwardingEntry(prefix)).first;
  }
  else
  {
    pruneExpiredHops(found->second, now);
  }

  const bool hopAdded =
      found->second.m_nextHops.emplace(port, PortNextHop{port, deadline}).second;
  return {prefixCreated, hopAdded};
}

void TemporaryForwardingTable::pruneExpiredHops(ForwardingEntry& entry, TimePoint now)
{
  for (auto hop = entry.m_nextHops.begin(); hop != entry.m_nextHops.end();)
  {
    if (now >= hop->second.deadline)
    {
      hop = entry.m_nextHops.erase(hop);
    }
    else
    {
      ++hop;
    }
  }
}

const ForwardingEntry* TemporaryForwardingTable::pruneAndFind(const ::ndn::Name& prefix,
                                                              TimePoint now)
{
  auto found = m_entries.find(prefix);
  if (found == m_entries.end())
  {
    return nullptr;
  }
  pruneExpiredHops(found->second, now);
  if (found->second.m_nextHops.empty())
  {
    m_entries.erase(found);
    return nullptr;
  }
  return &found->second;
}

const ForwardingEntry* TemporaryForwardingTable::findExact(const ::ndn::Name& prefix)
{
  return pruneAndFind(prefix, m_clock());
}

const ForwardingEntry* TemporaryForwardingTable::findLongestPrefix(const ::ndn::Name& name)
{
  const TimePoint now = m_clock();
  for (std::size_t count = name.size();; --count)
  {
    const ForwardingEntry* entry = pruneAndFind(name.getPrefix(count), now);
    if (entry != nullptr)
    {
      return entry;
    }
    if (count == 0)
    {
      break;
    }
  }
  return nullptr;
}

bool TemporaryForwardingTable::erase(const ::ndn::Name& prefix)
{
  return m_entries.erase(prefix) != 0;
}

std::size_t TemporaryForwardingTable::size() const
{
  return m_entries.size();
}

} // namespace statim
} // namespace ndn
} // namespace ns3
