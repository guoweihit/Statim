// SPDX-License-Identifier: GPL-3.0-or-later
// See the repository license and its additional permission for linking with ns-3.

#ifndef STATIM_TEMPORARY_FORWARDING_TABLE_HPP
#define STATIM_TEMPORARY_FORWARDING_TABLE_HPP

#include "state-types.hpp"

#include <ndn-cxx/name.hpp>

#include <cstddef>
#include <map>

namespace ns3
{
namespace ndn
{
namespace statim
{

struct PortNextHop
{
  Port port;
  TimePoint deadline;
};

class ForwardingEntry
{
public:
  const ::ndn::Name& getPrefix() const { return m_prefix; }
  const std::map<Port, PortNextHop>& getNextHops() const { return m_nextHops; }

private:
  friend class TemporaryForwardingTable;
  explicit ForwardingEntry(const ::ndn::Name& prefix) : m_prefix(prefix) {}

  ::ndn::Name m_prefix;
  std::map<Port, PortNextHop> m_nextHops;
};

/**
 * Temporary FIB for prefix-to-port forwarding state.
 *
 * Independent prefix-to-port state with lazy expiry and explicit lifetimes.
 * Controller generations and replacement policy belong to the caller, which
 * can erase a prefix before inserting its replacement.
 */
class TemporaryForwardingTable
{
public:
  struct InsertResult
  {
    bool prefixCreated;
    bool hopAdded;
  };

  explicit TemporaryForwardingTable(ClockFunction clock = StateClock::now);

  /** Prune an existing prefix, then add the port without renewing an active hop. */
  InsertResult insert(const ::ndn::Name& prefix, Port port, Lifetime lifetime);

  /** Insert with a fixed clock deadline; TimePoint::max() selects no finite lease. */
  InsertResult insertUntil(const ::ndn::Name& prefix, Port port, TimePoint deadline);

  /**
   * Lookups prune expired hops and remove empty prefixes. A returned view
   * references the stored hops and remains valid until the table's next operation.
   */
  const ForwardingEntry* findExact(const ::ndn::Name& prefix);
  const ForwardingEntry* findLongestPrefix(const ::ndn::Name& name);

  bool erase(const ::ndn::Name& prefix);

  /** Number of currently stored prefixes, including lazily expired entries. */
  std::size_t size() const;

private:
  static void pruneExpiredHops(ForwardingEntry& entry, TimePoint now);
  const ForwardingEntry* pruneAndFind(const ::ndn::Name& prefix, TimePoint now);

  ClockFunction m_clock;
  std::map<::ndn::Name, ForwardingEntry> m_entries;
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_TEMPORARY_FORWARDING_TABLE_HPP
