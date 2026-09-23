// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#ifndef STATIM_STATEFUL_MODULE_HPP
#define STATIM_STATEFUL_MODULE_HPP

#include "ns3/ndnSIM/model/ndn-common.hpp"
#include "ns3/ndnSIM/statim/control/local-route-controller.hpp"
#include "ns3/ndnSIM/statim/packet-header.hpp"
#include "ns3/ndnSIM/statim/pipeline/switch-pipeline.hpp"

namespace ns3
{
namespace ndn
{
namespace statim
{

class StatefulForwarder
{
public:
  struct Counters
  {

    uint64_t pullPlansCreated;
    uint64_t pendingInterestPullsPlanned;
    uint64_t tracePacketsParsed;
    uint64_t traceUpdatesAccepted;

    Counters()
        : pullPlansCreated(0), pendingInterestPullsPlanned(0), tracePacketsParsed(0),
          traceUpdatesAccepted(0)
    {
    }
  };

  StatefulForwarder();

  // --- Switch (flow table pipeline) ---

  SwitchPipeline& getSwitch() { return m_switch; }

  const SwitchPipeline& getSwitch() const { return m_switch; }

  // --- Temporary controller ---

  LocalRouteController& getTemporaryController() { return m_temporaryController; }

  const LocalRouteController& getTemporaryController() const { return m_temporaryController; }

  ::ns3::ndn::statim::TemporaryForwardingTable& getTemporaryFib()
  {
    return m_temporaryController.getTemporaryFib();
  }

  const ::ns3::ndn::statim::TemporaryForwardingTable& getTemporaryFib() const
  {
    return m_temporaryController.getTemporaryFib();
  }

  size_t getTemporaryFibSize() const { return m_temporaryController.fibSize(); }

  // --- Configuration ---

  void setEnableInterestReforwarding(bool enableInterestReforwarding) { m_enableInterestReforwarding = enableInterestReforwarding; }

  const Counters& getCounters() const { return m_counters; }

  // Normal KITE calls supply the saved TI's lifetime. The zero default supports
  // synthetic completion/lookup tests without a finite route lease.
  bool applyTraceUpdate(const ::ndn::Name& prefix, uint16_t portNumber, uint64_t seq,
                        ::ndn::time::microseconds ttl = ::ndn::time::microseconds::zero());

  static uint32_t encodeOutPortsMask(const std::set<uint16_t>& ports);

private:
  SwitchPipeline m_switch;
  LocalRouteController m_temporaryController;
  bool m_enableInterestReforwarding;
  Counters m_counters;
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_STATEFUL_MODULE_HPP
