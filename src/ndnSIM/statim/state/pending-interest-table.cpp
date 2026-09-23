// SPDX-License-Identifier: GPL-3.0-or-later
// See the repository license and its additional permission for linking with ns-3.

#include "pending-interest-table.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ns3
{
namespace ndn
{
namespace statim
{

PendingInterestTable::PendingInterestTable(ClockFunction clock) : m_clock(std::move(clock))
{
  if (!m_clock)
  {
    throw std::invalid_argument("PendingInterestTable requires a clock function");
  }
}

PendingInterestTable::InsertResult PendingInterestTable::insert(const ::ndn::Interest& interest,
                                                                Port port)
{
  auto found = m_entries.find(interest.getName());
  if (found != m_entries.end())
  {
    const InterestView& first = found->second.interest;
    if (first->hasNonce() && interest.hasNonce() && first->getNonce() == interest.getNonce())
    {
      return {InsertResult::Status::Duplicate, first};
    }

    const TimePoint now = m_clock();
    pruneExpiredIngress(found->second, now);
    found->second.ingress.push_back(
        {port, now, ::ndn::time::duration_cast<Lifetime>(interest.getInterestLifetime())});
    return {InsertResult::Status::Aggregated, first};
  }

  Entry entry;
  entry.interest = std::make_shared<::ndn::Interest>(interest);
  entry.ingress.push_back(
      {port, m_clock(), ::ndn::time::duration_cast<Lifetime>(interest.getInterestLifetime())});
  const InterestView remembered = entry.interest;
  m_entries.emplace(interest.getName(), std::move(entry));
  return {InsertResult::Status::Created, remembered};
}

void PendingInterestTable::pruneExpiredIngress(Entry& entry, TimePoint now)
{
  auto& ingress = entry.ingress;
  ingress.erase(std::remove_if(ingress.begin(), ingress.end(), [now](const IngressRecord& record)
                               { return now >= record.insertedAt + record.lifetime; }),
                ingress.end());
}

std::set<Port> PendingInterestTable::takeDataMatchPorts(const ::ndn::Name& dataName)
{
  std::set<Port> ports;
  for (std::size_t count = dataName.size();; --count)
  {
    auto found = m_entries.find(dataName.getPrefix(count));
    if (found != m_entries.end())
    {
      for (const auto& record : found->second.ingress)
      {
        ports.insert(record.port);
      }
      m_entries.erase(found);
    }
    if (count == 0)
    {
      break;
    }
  }
  return ports;
}

std::vector<PendingInterestTable::InterestView>
PendingInterestTable::findByPrefix(const ::ndn::Name& prefix) const
{
  std::vector<InterestView> interests;
  for (auto found = m_entries.lower_bound(prefix);
       found != m_entries.end() && prefix.isPrefixOf(found->first); ++found)
  {
    interests.push_back(found->second.interest);
  }
  return interests;
}

bool PendingInterestTable::erase(const ::ndn::Name& name)
{
  return m_entries.erase(name) != 0;
}

std::size_t PendingInterestTable::size() const
{
  return m_entries.size();
}

} // namespace statim
} // namespace ndn
} // namespace ns3
