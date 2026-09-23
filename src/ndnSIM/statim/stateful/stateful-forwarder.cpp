// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/ndnSIM/statim/stateful/stateful-forwarder.hpp"

namespace ns3
{
namespace ndn
{
namespace statim
{
StatefulForwarder::StatefulForwarder() : m_enableInterestReforwarding(false)
{
  m_switch.initPipeline();
}

bool StatefulForwarder::applyTraceUpdate(const ::ndn::Name& prefix, uint16_t portNumber,
                                         uint64_t seq, ::ndn::time::microseconds ttl)
{
  uint16_t port = portNumber;
  std::vector<uint8_t> nameHashes;
  PacketHeader tmpHeader(prefix);
  nameHashes = tmpHeader.getNameHashes();
  size_t prefixLen = prefix.size();
  if (prefixLen > STATIM_NAME_HASH_COUNT)
  {
    prefixLen = STATIM_NAME_HASH_COUNT;
  }

  bool updated =
      m_temporaryController.applyTraceUpdate(prefix, nameHashes, prefixLen, port, seq, ttl);

  if (updated)
  {
    ++m_counters.traceUpdatesAccepted;
  }
  return updated;
}

uint32_t StatefulForwarder::encodeOutPortsMask(const std::set<uint16_t>& ports)
{
  uint32_t mask = 0;
  for (uint16_t port : ports)
  {
    if (port >= 1 && port <= 32)
    {
      mask |= (1u << (32 - port));
    }
  }
  return mask;
}

} // namespace statim
} // namespace ndn
} // namespace ns3
