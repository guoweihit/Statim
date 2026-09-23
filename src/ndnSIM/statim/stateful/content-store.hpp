// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#ifndef STATIM_SIMPLE_CS_HPP
#define STATIM_SIMPLE_CS_HPP

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/name.hpp>

#include <cstddef>
#include <cstdlib>
#include <map>
#include <memory>

namespace ns3
{
namespace ndn
{
namespace statim
{

class ContentStore
{
public:
  // Zero capacity disables caching; shrinking evicts immediately.
  explicit ContentStore(size_t maxSize = 100) : m_maxSize(maxSize) {}

  std::shared_ptr<const ::ndn::Data> find(const ::ndn::Name& name) const
  {
    auto it = m_cache.find(name);
    if (it != m_cache.end())
    {
      return it->second;
    }
    return nullptr;
  }

  void insert(const ::ndn::Data& data)
  {
    if (m_maxSize == 0)
      return;
    // Data must be shared-owned, as in the ndnSIM packet receive path.
    const auto packet = data.shared_from_this();
    m_cache[data.getName()] = packet;
    trimToCapacity();
  }

  void clear() { m_cache.clear(); }

  size_t size() const { return m_cache.size(); }

  void setMaxSize(size_t maxSize)
  {
    m_maxSize = maxSize;
    trimToCapacity();
  }

private:
  void trimToCapacity()
  {
    if (m_maxSize == 0)
    {
      m_cache.clear();
      return;
    }
    // Retain the experiment's single coin-flip scan and RNG consumption.
    for (auto it = m_cache.begin(); it != m_cache.end() && m_cache.size() > m_maxSize;)
    {
      if (std::rand() % 2 == 1)
      {
        it = m_cache.erase(it);
      }
      else
      {
        ++it;
      }
    }
    // A scan can keep every entry. Finish deterministically to enforce the
    // bound without consuming further random numbers.
    while (m_cache.size() > m_maxSize)
    {
      m_cache.erase(m_cache.begin());
    }
  }

  std::map<::ndn::Name, std::shared_ptr<const ::ndn::Data>> m_cache;
  size_t m_maxSize;
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_SIMPLE_CS_HPP
