// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/ndnSIM/statim/packet-header.hpp"

#include <algorithm>
#include <functional>
#include <ostream>
#include <string>

namespace
{
inline bool isValidPacketType(uint8_t value)
{
  return value == 0x00 || value == 0x01 || value == 0x02 || value == 0x03 || value == 0x05;
}
} // namespace

namespace ns3
{
namespace ndn
{
namespace statim
{

ns3::TypeId PacketHeader::GetTypeId()
{
  static ns3::TypeId tid = ns3::TypeId("ns3::ndn::statim::PacketHeader")
                               .SetGroupName("Ndn")
                               .SetParent<Header>()
                               .AddConstructor<PacketHeader>();
  return tid;
}

TypeId PacketHeader::GetInstanceTypeId() const
{
  return GetTypeId();
}

PacketHeader::PacketHeader()
    : m_type(PacketType::UNPROCESSED), m_outPorts(0), m_packetInPort(0),
      m_nameHashes(STATIM_NAME_HASH_COUNT, 0xff), m_switchId(0)
{
}

PacketHeader::PacketHeader(const ::ndn::Name& name)
    : m_type(PacketType::UNPROCESSED), m_outPorts(0), m_packetInPort(0),
      m_nameHashes(STATIM_NAME_HASH_COUNT, 0xff), m_switchId(0)
{
  computeNameHashes(name);
}

uint32_t PacketHeader::GetSerializedSize() const
{
  return SERIALIZED_SIZE;
}

void PacketHeader::Serialize(ns3::Buffer::Iterator start) const
{
  start.WriteU8(static_cast<uint8_t>(m_type));
  start.WriteHtonU32(m_outPorts);
  start.WriteHtonU16(m_packetInPort);
  for (size_t i = 0; i < STATIM_NAME_HASH_COUNT; ++i)
  {
    start.WriteU8(m_nameHashes[i]);
  }
  start.WriteU8(m_switchId);
}

uint32_t PacketHeader::Deserialize(ns3::Buffer::Iterator start)
{
  clear();

  // Probe a copied iterator to determine the available bytes from the
  // caller's current position within the ns-3 buffer.
  auto probe = start;
  for (uint32_t i = 0; i < SERIALIZED_SIZE; ++i)
  {
    if (probe.IsEnd())
    {
      return 0;
    }
    probe.Next();
  }

  uint8_t type = start.ReadU8();
  m_type = static_cast<PacketType>(type);
  if (!isValidPacketType(type))
  {
    return 0;
  }
  m_outPorts = start.ReadNtohU32();
  m_packetInPort = start.ReadNtohU16();
  for (size_t i = 0; i < STATIM_NAME_HASH_COUNT; ++i)
  {
    m_nameHashes[i] = start.ReadU8();
  }
  m_switchId = start.ReadU8();

  return SERIALIZED_SIZE;
}

void PacketHeader::Print(std::ostream& os) const
{
  os << "PacketHeader(type=" << static_cast<uint32_t>(m_type) << ", inPort=" << m_packetInPort
     << ", outPorts=0x" << std::hex << m_outPorts << std::dec
     << ", switchId=" << static_cast<uint32_t>(m_switchId) << ", hashes=" << getNameHashCount()
     << ")";
}

void PacketHeader::clear()
{
  m_type = PacketType::UNPROCESSED;
  m_outPorts = 0;
  m_packetInPort = 0;
  m_nameHashes.assign(STATIM_NAME_HASH_COUNT, 0xff);
  m_switchId = 0;
}

bool PacketHeader::isValid() const
{
  return isValidPacketType(static_cast<uint8_t>(m_type)) &&
         m_nameHashes.size() == STATIM_NAME_HASH_COUNT;
}

size_t PacketHeader::getNameHashCount() const
{
  size_t count = 0;
  for (size_t i = 0; i < m_nameHashes.size(); ++i)
  {
    if (m_nameHashes[i] != 0xff)
    {
      ++count;
    }
    else
    {
      break;
    }
  }
  return count;
}

uint8_t PacketHeader::computeComponentHash(const ::ndn::name::Component& component)
{
  std::hash<std::string> hasher;
  size_t h = hasher(component.toUri());
  return static_cast<uint8_t>(h % 0xfe);
}

void PacketHeader::computeNameHashes(const ::ndn::Name& name)
{
  m_nameHashes.assign(STATIM_NAME_HASH_COUNT, 0xff);
  size_t count = std::min(name.size(), STATIM_NAME_HASH_COUNT);
  for (size_t i = 0; i < count; ++i)
  {
    m_nameHashes[i] = computeComponentHash(name.get(i));
  }
}

} // namespace statim
} // namespace ndn
} // namespace ns3
