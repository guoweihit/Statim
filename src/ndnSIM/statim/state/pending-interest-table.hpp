// SPDX-License-Identifier: GPL-3.0-or-later
// See the repository license and its additional permission for linking with ns-3.

#ifndef STATIM_PENDING_INTEREST_TABLE_HPP
#define STATIM_PENDING_INTEREST_TABLE_HPP

#include "state-types.hpp"

#include <ndn-cxx/interest.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace ns3
{
namespace ndn
{
namespace statim
{

/**
 * Name-based pending state for the recorded simulation model.
 *
 * Ingress expiry is checked when an Interest aggregates. Data satisfaction
 * consumes every matching stored ingress, even if its lifetime has elapsed.
 * This simulation container performs cleanup during Interest aggregation and
 * Data satisfaction.
 */
class PendingInterestTable
{
public:
  using InterestView = std::shared_ptr<const ::ndn::Interest>;

  struct InsertResult
  {
    enum class Status
    {
      Created,
      Aggregated,
      Duplicate
    };

    Status status;
    InterestView interest;

    bool accepted() const { return status != Status::Duplicate; }
    bool created() const { return status == Status::Created; }
    bool aggregated() const { return status == Status::Aggregated; }
    bool duplicate() const { return status == Status::Duplicate; }
  };

  explicit PendingInterestTable(ClockFunction clock = StateClock::now);

  /** Copy the first accepted Interest; caller ownership is unrestricted. */
  InsertResult insert(const ::ndn::Interest& interest, Port port);

  /** Consume all ancestor-name entries, including the root, without TTL pruning. */
  std::set<Port> takeDataMatchPorts(const ::ndn::Name& dataName);

  /**
   * List remembered Interests in Name order without consuming or pruning them.
   * Each Name has one remembered Interest, with ordering determined by Name.
   * Returned shared views remain valid after the table changes or is destroyed.
   */
  std::vector<InterestView> findByPrefix(const ::ndn::Name& prefix) const;

  bool erase(const ::ndn::Name& name);
  std::size_t size() const;

private:
  struct IngressRecord
  {
    Port port;
    TimePoint insertedAt;
    Lifetime lifetime;
  };

  struct Entry
  {
    InterestView interest;
    std::vector<IngressRecord> ingress;
  };

  /** Isolated model policy: called on accepted aggregation only. */
  static void pruneExpiredIngress(Entry& entry, TimePoint now);

  ClockFunction m_clock;
  std::map<::ndn::Name, Entry> m_entries;
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_PENDING_INTEREST_TABLE_HPP
