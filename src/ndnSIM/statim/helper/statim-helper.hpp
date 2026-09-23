/**
 * Copyright (c) 2011-2015  Regents of the University of California.
 *
 * This file is part of ndnSIM. See AUTHORS for complete list of ndnSIM authors and
 * contributors.
 *
 * ndnSIM is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * ndnSIM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ndnSIM, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 **/

// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for the Statim simulation artifact, 2026-09-12.
// Upstream-derived portions: see THIRD-PARTY.md (repository root).
// Statim modifications and additional linking permission: see LICENSE.md.

#ifndef NDNSIM_HELPER_STATIM_HELPER_HPP
#define NDNSIM_HELPER_STATIM_HELPER_HPP

#include "ns3/ndnSIM/model/ndn-common.hpp"

#include <string>

#include "ns3/ndnSIM/helper/ndn-stack-helper.hpp"

namespace ns3
{
namespace ndn
{

class StatimHelper : boost::noncopyable
{
public:
  StatimHelper();
  ~StatimHelper();

  void SetDefaultRoutes(bool needSet);

  void SetControllerDelay(Time delay);

  Time GetControllerDelay() const { return m_controllerDelay; }

  void SetEnableInterestReforwarding(bool enableInterestReforwarding);

  bool GetEnableInterestReforwarding() const { return m_enableInterestReforwarding; }

  void SetEnableTemporaryFib(bool enable);

  bool GetEnableTemporaryFib() const { return m_enableTemporaryFib; }

  void SetFlowTableFibEntryLifetime(Time lifetime) { m_flowTableFibEntryLifetime = lifetime; }

  Time GetFlowTableFibEntryLifetime() const { return m_flowTableFibEntryLifetime; }

  void SetTemporaryFibEntryLifetime(Time lifetime) { m_temporaryFibEntryLifetime = lifetime; }

  Time GetTemporaryFibEntryLifetime() const { return m_temporaryFibEntryLifetime; }

  void SetStatefulDelay(Time delay) { m_statefulDelay = delay; }

  Time GetStatefulDelay() const { return m_statefulDelay; }

  void SetInterestReforwardingLimit(int n) { m_interestReforwardingLimit = n; }

  int GetInterestReforwardingLimit() const { return m_interestReforwardingLimit; }

  void SetControllerRetryTimeout(Time t) { m_controllerRetryTimeout = t; }

  void SetControllerRetryLimit(int n) { m_controllerRetryLimit = n; }

  void SetGenerationGuardEnabled(bool enabled) { m_generationGuardEnabled = enabled; }

  void SetSyncAckLossRate(double p) { m_syncAckLossRate = p; }

  void SetEnableStatimPacket(bool enable);

  // Enable TI/TD path and first-steer telemetry; disabled by default.
  void SetExperimentTelemetryEnabled(bool enable) { m_experimentTelemetryEnabled = enable; }

  void SetOldContentStore(const std::string& contentStoreClass, const std::string& attr1 = "",
                          const std::string& value1 = "", const std::string& attr2 = "",
                          const std::string& value2 = "", const std::string& attr3 = "",
                          const std::string& value3 = "", const std::string& attr4 = "",
                          const std::string& value4 = "");

  void setCsSize(size_t maxSize);

  void setPolicy(const std::string& policy);

  Ptr<FaceContainer> Install(Ptr<Node> node) const;

  Ptr<FaceContainer> Install(const NodeContainer& c) const;

  Ptr<FaceContainer> InstallAll() const;

  StackHelper& GetStackHelper() { return m_stackHelper; }

  const StackHelper& GetStackHelper() const { return m_stackHelper; }

  static void CalculateStatimRoutes();

private:
  bool ShouldEnableStatimPacketOnNode(Ptr<Node> node) const;

  static void ConfigureInstalledNode(Ptr<Node> node, Time controllerDelay, bool enableInterestReforwarding,
                                     bool enableTemporaryFib, Time statefulDelay, int interestReforwardingLimit,
                                     Time controllerRetryTimeout, int controllerRetryLimit,
                                     bool generationGuardEnabled, double syncAckLossRate,
                                     bool experimentTelemetryEnabled);

  static void ConfigureEntryLifetimes(Ptr<Node> node, Time flowTableFibEntryLifetime,
                                      Time temporaryFibEntryLifetime);

private:
  mutable StackHelper m_stackHelper;
  Time m_controllerDelay;
  Time m_flowTableFibEntryLifetime;
  Time m_temporaryFibEntryLifetime;
  Time m_statefulDelay;
  Time m_controllerRetryTimeout;
  bool m_enableInterestReforwarding;
  bool m_enableTemporaryFib;
  bool m_enableStatimPacket;
  bool m_experimentTelemetryEnabled;
  bool m_generationGuardEnabled;
  int m_interestReforwardingLimit;
  int m_controllerRetryLimit;
  double m_syncAckLossRate;
};

} // namespace ndn
} // namespace ns3

#endif // NDNSIM_HELPER_STATIM_HELPER_HPP
