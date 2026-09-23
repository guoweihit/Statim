// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#ifndef STATIM_CONTROL_MESSAGE_HPP
#define STATIM_CONTROL_MESSAGE_HPP

#include "ns3/nstime.h"

#include <cstdint>
#include <ndn-cxx/name.hpp>
#include <vector>

namespace ns3
{
namespace ndn
{
namespace statim
{

enum class ControlMessageType
{
  PACKET_IN = 0,
  FLOW_MOD,
  SYNC_ACK
};

struct ControlMessage
{
  ControlMessageType type;

  std::vector<uint8_t> nameHashes;
  size_t prefixLen;
  uint16_t outputPort;

  ::ndn::Name prefix;
  uint64_t seq;
  // Absolute simulator deadline fixed when the local routing decision is
  // admitted. Zero selects a lease-free synthetic update used by state tests.
  Time routeExpiry;

  ControlMessage()
      : type(ControlMessageType::PACKET_IN), prefixLen(0), outputPort(0), seq(0),
        routeExpiry(Seconds(0))
  {
  }

  ControlMessage(ControlMessageType t, const std::vector<uint8_t>& hashes, size_t pLen,
                 uint16_t port, const ::ndn::Name& pfx, uint64_t s = 0, Time expiry = Seconds(0))
      : type(t), nameHashes(hashes), prefixLen(pLen), outputPort(port), prefix(pfx), seq(s),
        routeExpiry(expiry)
  {
  }

  bool isExpiredAt(Time now) const
  {
    return routeExpiry != Seconds(0) && now >= routeExpiry;
  }
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_CONTROL_MESSAGE_HPP
