// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#ifndef STATIM_HEADER_HPP
#define STATIM_HEADER_HPP

#include "ns3/header.h"
#include <ndn-cxx/name.hpp>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace ns3
{
namespace ndn
{
namespace statim
{

static const size_t STATIM_NAME_HASH_COUNT = 4;

enum class PacketType : uint8_t
{
  UNPROCESSED = 0x00,
  INTEREST = 0x01,
  DATA = 0x02,
  CONTROL = 0x03,
  SELF_LEARNING_DISCOVERY = 0x05
};

class PacketHeader : public Header
{
public:
  // type(1B) + out_ports(4B) + in_port(2B) + hashes(4B) + switch_id(1B).
  static const uint32_t SERIALIZED_SIZE = 1 + 4 + 2 + STATIM_NAME_HASH_COUNT + 1;

  static ns3::TypeId GetTypeId();

  TypeId GetInstanceTypeId() const override;

  PacketHeader();

  explicit PacketHeader(const ::ndn::Name& name);

  uint32_t GetSerializedSize() const override;

  void Serialize(ns3::Buffer::Iterator start) const override;

  // Returns zero for truncated input or an unknown packet type.
  uint32_t Deserialize(ns3::Buffer::Iterator start) override;

  void Print(std::ostream& os) const override;

  void clear();

  bool isValid() const;

  PacketType getType() const { return m_type; }

  void setType(PacketType type) { m_type = type; }

  void setOutPorts(uint32_t ports) { m_outPorts = ports; }

  void setPacketInPort(uint16_t port) { m_packetInPort = port; }

  uint8_t getSwitchId() const { return m_switchId; }

  void setSwitchId(uint8_t id) { m_switchId = id; }

  size_t getNameHashCount() const;

  const std::vector<uint8_t>& getNameHashes() const { return m_nameHashes; }

  static uint8_t computeComponentHash(const ::ndn::name::Component& component);

  void computeNameHashes(const ::ndn::Name& name);

private:
  PacketType m_type;
  uint32_t m_outPorts;
  uint16_t m_packetInPort;
  std::vector<uint8_t> m_nameHashes;
  uint8_t m_switchId;
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_HEADER_HPP
