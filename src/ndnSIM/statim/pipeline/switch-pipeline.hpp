// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#ifndef STATIM_SWITCH_HPP
#define STATIM_SWITCH_HPP

#include "ns3/ndnSIM/statim/packet-header.hpp"
#include "ns3/ndnSIM/statim/pipeline/flow-table.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ns3
{
namespace ndn
{
namespace statim
{

namespace StatimPort
{
static const uint16_t STATEFUL_MODULE = 0xFFFF;
static const uint16_t CONTROLLER = 0xFFFD;
static const uint16_t DROP = 0x0000;
} // namespace StatimPort

namespace StatimTableId
{
static const uint8_t NDN_STATE = 50;
// Flow-table FIB: maps encoded name components to forwarding output ports.
static const uint8_t NDN_FIB = 51;
static const uint8_t MULTI_BYTE1 = 52;
static const uint8_t MULTI_BYTE2 = 53;
static const uint8_t MULTI_BYTE3 = 54;
static const uint8_t MULTI_BYTE4 = 55;
} // namespace StatimTableId

class SwitchPipeline
{
public:
  struct Counters
  {
    uint64_t pipelineProcessed;
    uint64_t fibLookupHits;
    uint64_t fibLookupMisses;
    uint64_t packetInsSent;
    uint64_t flowModsApplied;

    Counters()
        : pipelineProcessed(0), fibLookupHits(0), fibLookupMisses(0), packetInsSent(0),
          flowModsApplied(0)
    {
    }
  };

  // Read-only observation of the entry selected by table 51 for deterministic
  // validation of its matched prefix length and output port.
  struct FibLookupDiagnostic
  {
    bool matched;
    size_t matchLength;
    uint16_t outputPort;

    FibLookupDiagnostic() : matched(false), matchLength(0), outputPort(0) {}
  };

  SwitchPipeline();

  void initPipeline();

  PipelineResult processPacket(const uint8_t* headerBuf, size_t bufLen) const;

  PipelineResult processPacket(const PacketHeader& header) const;

  void installFibEntry(const std::vector<uint8_t>& nameHashes, uint16_t outputPort,
                       Time expiry = Seconds(0));

  void removeFibEntry(const std::vector<uint8_t>& nameHashes);

  FibLookupDiagnostic diagnoseFibLookup(const PacketHeader& header) const;

  const Counters& getCounters() const { return m_counters; }

private:
  static const uint32_t FIB_PRIORITY_BASE = 2000;
  static const uint32_t FIB_PRIORITY_STEP = 1000;

  static uint32_t fibPriority(const std::vector<uint8_t>& nameHashes);

  void initMulticastByteTable(uint8_t tableId, int byteIndex, uint8_t nextTableId, bool isLast);

  FlowTablePipeline m_pipeline;
  mutable Counters m_counters;
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_SWITCH_HPP
