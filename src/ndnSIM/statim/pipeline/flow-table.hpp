// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#ifndef STATIM_FLOW_TABLE_HPP
#define STATIM_FLOW_TABLE_HPP

#include "ns3/nstime.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ns3
{
namespace ndn
{
namespace statim
{

// Match offsets and lengths are in bits, ordered most significant bit first.
struct MatchField
{
  uint16_t offset;
  uint16_t length;
  uint32_t value;
  bool wildcard;

  MatchField() : offset(0), length(0), value(0), wildcard(true) {}

  MatchField(uint16_t bitOffset, uint16_t bitLength, uint32_t val)
      : offset(bitOffset), length(bitLength), value(val), wildcard(false)
  {
  }

  static MatchField Wildcard(uint16_t bitOffset, uint16_t bitLength)
  {
    MatchField m;
    m.offset = bitOffset;
    m.length = bitLength;
    m.wildcard = true;
    return m;
  }

  bool matches(const uint8_t* headerBuf, size_t bufLenBytes) const
  {
    if (headerBuf == nullptr || length == 0 || length > 32)
      return false;
    const size_t endBit = static_cast<size_t>(offset) + length;
    if ((endBit + 7) / 8 > bufLenBytes)
      return false;
    if (wildcard)
      return true;
    return extractBits(headerBuf, offset, length) == value;
  }

private:
  static uint32_t extractBits(const uint8_t* buf, uint16_t startBit, uint16_t numBits)
  {
    uint32_t result = 0;
    for (uint16_t i = 0; i < numBits; ++i)
    {
      size_t bitPos = static_cast<size_t>(startBit) + i;
      size_t byteIdx = bitPos / 8;
      uint8_t bitInByte = 7 - (bitPos % 8);
      if (buf[byteIdx] & (1 << bitInByte))
      {
        result |= (1u << (numBits - 1 - i));
      }
    }
    return result;
  }
};

enum class FlowActionType : uint8_t
{
  OUTPUT,
  OUTPUT_MULTICAST,
  GOTO_TABLE,
  PACKET_IN,
  SET_FIELD,
  DROP
};

struct FlowAction
{
  FlowActionType type;
  uint16_t portNumber;
  uint8_t gotoTableId;
  uint16_t setFieldOffset;
  uint16_t setFieldLength;
  std::vector<uint8_t> setFieldValue;

  FlowAction()
      : type(FlowActionType::DROP), portNumber(0), gotoTableId(0), setFieldOffset(0),
        setFieldLength(0)
  {
  }

  static FlowAction Output(uint16_t port)
  {
    FlowAction a;
    a.type = FlowActionType::OUTPUT;
    a.portNumber = port;
    return a;
  }

  static FlowAction OutputMulticast(uint16_t port)
  {
    FlowAction a;
    a.type = FlowActionType::OUTPUT_MULTICAST;
    a.portNumber = port;
    return a;
  }

  static FlowAction GotoTable(uint8_t tableId)
  {
    FlowAction a;
    a.type = FlowActionType::GOTO_TABLE;
    a.gotoTableId = tableId;
    return a;
  }

  static FlowAction PacketIn()
  {
    FlowAction a;
    a.type = FlowActionType::PACKET_IN;
    return a;
  }

  // Unlike match fields, SET_FIELD offsets and lengths are in bytes.
  static FlowAction SetField(uint16_t offset, uint16_t length, const std::vector<uint8_t>& val)
  {
    FlowAction a;
    a.type = FlowActionType::SET_FIELD;
    a.setFieldOffset = offset;
    a.setFieldLength = length;
    a.setFieldValue = val;
    return a;
  }

  static FlowAction Drop()
  {
    FlowAction a;
    a.type = FlowActionType::DROP;
    return a;
  }
};

struct FlowEntry
{
  std::vector<MatchField> matchFields;
  uint32_t priority;
  std::vector<FlowAction> actions;
  Time expiry;
  bool hasExpiry = false;

  FlowEntry() : priority(0) {}

  bool isExpired() const { return hasExpiry && Simulator::Now() >= expiry; }

  bool matches(const uint8_t* headerBuf, size_t bufLen) const
  {
    if (isExpired())
      return false;
    for (const auto& field : matchFields)
    {
      if (!field.matches(headerBuf, bufLen))
        return false;
    }
    return true;
  }
};

class FlowTable
{
public:
  explicit FlowTable(uint8_t tableId = 0, const std::string& name = "");

  uint8_t getTableId() const { return m_tableId; }

  void addEntry(const FlowEntry& entry);

  template <typename Predicate> void removeIf(Predicate pred)
  {
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), pred), m_entries.end());
  }

  void clear();

  const FlowEntry* lookup(const uint8_t* headerBuf, size_t bufLen) const;

private:
  void sortEntries();

  uint8_t m_tableId;
  std::string m_name;
  std::vector<FlowEntry> m_entries;
};

struct PipelineResult
{
  bool matched;
  bool packetIn;
  bool dropped;
  std::set<uint16_t> outputPorts;
  std::vector<uint8_t> modifiedHeader;

  PipelineResult() : matched(false), packetIn(false), dropped(false) {}
};

class FlowTablePipeline
{
public:
  FlowTablePipeline();

  FlowTable& getTable(uint8_t tableId);
  const FlowTable& getTable(uint8_t tableId) const;
  FlowTable& addTable(uint8_t tableId, const std::string& name = "");

  PipelineResult process(const uint8_t* headerBuf, size_t bufLen, uint8_t startTableId = 0) const;

private:
  std::vector<FlowTable> m_tables;
};

//  |  type  | out_ports(32bit) | in_port(16bit) | hash[0-3] | sw_id |
//  | 8 bits |     32 bits      |    16 bits     |  4×8 bits | 8 bits|
//  bit 0                                                     bit 95

namespace StatimField
{
static const uint16_t TYPE_OFFSET = 0;
static const uint16_t TYPE_LENGTH = 8;

static const uint16_t OUT_PORTS_OFFSET = 8;
static const uint16_t OUT_PORTS_LENGTH = 32;

static const uint16_t IN_PORT_OFFSET = 40;
static const uint16_t IN_PORT_LENGTH = 16;

static const uint16_t NAME_HASH_BASE_OFFSET = 56;
static const uint16_t NAME_HASH_LENGTH = 8;

static const uint16_t SWITCH_ID_OFFSET = 88;
static const uint16_t SWITCH_ID_LENGTH = 8;

inline uint16_t nameHashOffset(size_t idx)
{
  if (idx >= 4)
    throw std::out_of_range("name hash index must be below four");
  return static_cast<uint16_t>(NAME_HASH_BASE_OFFSET + idx * NAME_HASH_LENGTH);
}
} // namespace StatimField

inline MatchField matchType(uint8_t typeValue)
{
  return MatchField(StatimField::TYPE_OFFSET, StatimField::TYPE_LENGTH, typeValue);
}

inline MatchField matchTypeWildcard()
{
  return MatchField::Wildcard(StatimField::TYPE_OFFSET, StatimField::TYPE_LENGTH);
}

inline MatchField matchOutPortsZero()
{
  return MatchField(StatimField::OUT_PORTS_OFFSET, StatimField::OUT_PORTS_LENGTH, 0x00000000u);
}

inline MatchField matchOutPortsWildcard()
{
  return MatchField::Wildcard(StatimField::OUT_PORTS_OFFSET, StatimField::OUT_PORTS_LENGTH);
}

inline MatchField matchOutPortByte(size_t byteIdx, uint8_t value)
{
  if (byteIdx >= 4)
    throw std::out_of_range("output port byte index must be below four");
  return MatchField(static_cast<uint16_t>(StatimField::OUT_PORTS_OFFSET + byteIdx * 8), 8, value);
}

inline MatchField matchNameHash(size_t idx, uint8_t hashValue)
{
  return MatchField(StatimField::nameHashOffset(idx), StatimField::NAME_HASH_LENGTH, hashValue);
}

inline MatchField matchNameHashWildcard(size_t idx)
{
  return MatchField::Wildcard(StatimField::nameHashOffset(idx), StatimField::NAME_HASH_LENGTH);
}

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_FLOW_TABLE_HPP
