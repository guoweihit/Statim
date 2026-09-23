// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#include "ns3/core-module.h"
#include "ns3/ndnSIM/statim/control/flow-table-controller.hpp"
#include "ns3/ndnSIM/statim/stateful/stateful-forwarder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ns3;
using namespace ns3::ndn::statim;

namespace
{

struct Sample
{
  uint64_t id;
  double arrival;
  double completion;
  double residence;
};

class TemporaryFibExperiment
{
public:
  explicit TemporaryFibExperiment(double tc)
      : m_lastObservedSize(0), m_highWater(0), m_lastTransition(0.0), m_area(0.0),
        m_controlRequestsObserved(0), m_flowModsObserved(0), m_syncCompleteCallbacks(0),
        m_insertLookupChecks(0), m_deleteLookupChecks(0)
  {
    m_module.getTemporaryController().setControllerDelay(Seconds(tc));
    m_controller.setControllerDelay(Seconds(tc));
    m_controller.setSyncAckLossRate(0.0);

    m_module.getTemporaryController().setFlowTableSyncRequestCallback(
        [this](const ControlMessage& message)
        {
          if (message.type != ControlMessageType::PACKET_IN)
          {
            throw std::runtime_error("temporary controller emitted a non-PACKET_IN request");
          }
          ++m_controlRequestsObserved;
          m_controller.submitFlowUpdate(message);
        });
    m_controller.setFlowModApplyCallback(
        [this](const ControlMessage& message)
        {
          if (message.type != ControlMessageType::FLOW_MOD)
          {
            throw std::runtime_error("central controller emitted a non-FLOW_MOD update");
          }
          if (m_module.getTemporaryFib().findExact(message.prefix) == nullptr)
          {
            throw std::runtime_error("temporary entry disappeared before FLOW_MOD completion");
          }
          ++m_flowModsObserved;
          return true;
        });
    m_controller.setSyncAckCallback([this](const ControlMessage& message) { Complete(message); });
    m_module.getTemporaryController().setSyncCompleteCallback(
        [this](const ::ndn::Name& prefix)
        {
          if (m_module.getTemporaryFib().findExact(prefix) != nullptr)
          {
            throw std::runtime_error("completion callback observed a resident entry");
          }
          ++m_syncCompleteCallbacks;
        });
  }

  void Arrive(uint64_t id)
  {
    const double now = Simulator::Now().GetSeconds();
    AccountUntil(now);
    VerifyRealSize("before arrival");

    ::ndn::Name prefix("/statim/temporary-fib-occupancy/prefix");
    prefix.appendNumber(id);
    if (m_arrivals.find(prefix) != m_arrivals.end())
    {
      throw std::runtime_error("duplicate generated prefix");
    }

    const size_t before = m_module.getTemporaryFibSize();
    const uint16_t port = static_cast<uint16_t>((id % 32) + 1);
    // This arrival/completion experiment removes entries on successful
    // completion and explicitly selects no independent finite route lease.
    if (!m_module.applyTraceUpdate(prefix, port, id + 1, ::ndn::time::microseconds::zero()))
    {
      throw std::runtime_error("unique trace update was rejected");
    }
    const size_t after = m_module.getTemporaryFibSize();
    if (after != before + 1)
    {
      throw std::runtime_error("temporary FIB size did not increase after insertion");
    }
    if (m_module.getTemporaryFib().findExact(prefix) == nullptr)
    {
      throw std::runtime_error("inserted prefix is not exactly queryable");
    }
    ::ndn::Name probe(prefix);
    probe.append("probe");
    if (m_module.getTemporaryFib().findLongestPrefix(probe) == nullptr)
    {
      throw std::runtime_error("inserted prefix is not longest-prefix queryable");
    }

    ++m_insertLookupChecks;
    m_arrivals[prefix] = std::make_pair(id, now);
    m_lastObservedSize = after;
    m_highWater = std::max(m_highWater, static_cast<uint64_t>(after));
  }

  void Finish(double endTime, uint32_t arrivalCount)
  {
    AccountUntil(endTime);
    VerifyRealSize("at finish");
    if (m_module.getTemporaryFibSize() != 0 || !m_arrivals.empty())
    {
      throw std::runtime_error("experiment ended with resident prefixes");
    }
    if (m_samples.size() != arrivalCount)
    {
      throw std::runtime_error("not all prefixes produced residence samples");
    }

    const StatefulForwarder::Counters& module = m_module.getCounters();
    const LocalRouteController::Counters& temp = m_module.getTemporaryController().getCounters();
    const FlowTableController::Counters& central = m_controller.getCounters();
    if (module.traceUpdatesAccepted != arrivalCount || temp.tempUpdates != arrivalCount ||
        temp.controlPackets != arrivalCount || temp.syncCompletions != arrivalCount ||
        temp.controllerRetries != 0 || temp.temporaryRouteWithdrawals != 0 || central.packetIns != arrivalCount ||
        central.flowModsApplied != arrivalCount || central.syncAcksDropped != 0 ||
        m_controlRequestsObserved != arrivalCount || m_flowModsObserved != arrivalCount ||
        m_syncCompleteCallbacks != arrivalCount || m_insertLookupChecks != arrivalCount ||
        m_deleteLookupChecks != arrivalCount)
    {
      throw std::runtime_error("implementation counters are inconsistent with arrival count");
    }
  }

  uint64_t HighWater() const { return m_highWater; }
  double Area() const { return m_area; }
  const std::vector<Sample>& Samples() const { return m_samples; }
  const StatefulForwarder& Module() const { return m_module; }
  const FlowTableController& Controller() const { return m_controller; }
  uint64_t ControlRequestsObserved() const { return m_controlRequestsObserved; }
  uint64_t FlowModsObserved() const { return m_flowModsObserved; }
  uint64_t SyncCompleteCallbacks() const { return m_syncCompleteCallbacks; }
  uint64_t InsertLookupChecks() const { return m_insertLookupChecks; }
  uint64_t DeleteLookupChecks() const { return m_deleteLookupChecks; }

private:
  void Complete(const ControlMessage& message)
  {
    if (message.type != ControlMessageType::SYNC_ACK)
    {
      throw std::runtime_error("central controller emitted a non-SYNC_ACK completion");
    }
    const double now = Simulator::Now().GetSeconds();
    AccountUntil(now);
    VerifyRealSize("before completion");

    std::map<::ndn::Name, std::pair<uint64_t, double>>::iterator arrival =
        m_arrivals.find(message.prefix);
    if (arrival == m_arrivals.end())
    {
      throw std::runtime_error("SYNC_ACK has no matching resident prefix");
    }
    if (m_module.getTemporaryFib().findExact(message.prefix) == nullptr)
    {
      throw std::runtime_error("prefix is not queryable immediately before SYNC_ACK");
    }

    const size_t before = m_module.getTemporaryFibSize();
    if (!m_module.getTemporaryController().handleControlResponse(message))
    {
      throw std::runtime_error("matching SYNC_ACK was rejected");
    }
    const size_t after = m_module.getTemporaryFibSize();
    if (before == 0 || after + 1 != before)
    {
      throw std::runtime_error("temporary FIB size did not decrease after SYNC_ACK");
    }
    if (m_module.getTemporaryFib().findExact(message.prefix) != nullptr)
    {
      throw std::runtime_error("deleted prefix remains exactly queryable");
    }

    const uint64_t id = arrival->second.first;
    const double arrivalTime = arrival->second.second;
    Sample sample = {id, arrivalTime, now, now - arrivalTime};
    m_samples.push_back(sample);
    m_arrivals.erase(arrival);
    ++m_deleteLookupChecks;
    m_lastObservedSize = after;
  }

  void AccountUntil(double now)
  {
    if (now + 1e-12 < m_lastTransition)
    {
      throw std::runtime_error("simulation time moved backwards");
    }
    m_area += static_cast<double>(m_lastObservedSize) * (now - m_lastTransition);
    m_lastTransition = now;
  }

  void VerifyRealSize(const char* context) const
  {
    if (m_module.getTemporaryFibSize() != m_lastObservedSize)
    {
      throw std::runtime_error(std::string("accounting diverged from temporary FIB size ") + context);
    }
  }

  size_t m_lastObservedSize;
  uint64_t m_highWater;
  double m_lastTransition;
  double m_area;
  StatefulForwarder m_module;
  FlowTableController m_controller;
  std::map<::ndn::Name, std::pair<uint64_t, double>> m_arrivals;
  std::vector<Sample> m_samples;
  uint64_t m_controlRequestsObserved;
  uint64_t m_flowModsObserved;
  uint64_t m_syncCompleteCallbacks;
  uint64_t m_insertLookupChecks;
  uint64_t m_deleteLookupChecks;
};

double Percentile(std::vector<double> values, double p)
{
  if (values.empty())
    return 0.0;
  std::sort(values.begin(), values.end());
  const double rank = p * static_cast<double>(values.size() - 1);
  const size_t lower = static_cast<size_t>(std::floor(rank));
  const size_t upper = static_cast<size_t>(std::ceil(rank));
  const double fraction = rank - static_cast<double>(lower);
  return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

std::string JsonEscape(const std::string& input)
{
  std::ostringstream out;
  for (std::string::const_iterator it = input.begin(); it != input.end(); ++it)
  {
    if (*it == '\\' || *it == '"')
      out << '\\';
    out << *it;
  }
  return out.str();
}

} // namespace

int main(int argc, char* argv[])
{
  double lambda = 10.0;
  uint32_t arrivalCount = 1000;
  double tc = 0.1;
  uint32_t burst = 0;
  uint32_t seed = 1;
  std::string outputPrefix = "temporary-fib-occupancy";

  CommandLine cmd;
  cmd.AddValue("lambda", "Poisson arrival rate after the initial burst (prefixes/s)", lambda);
  cmd.AddValue("arrivalCount", "Total unique prefixes, including the initial burst", arrivalCount);
  cmd.AddValue("Tc", "Deterministic central-controller completion delay in seconds", tc);
  cmd.AddValue("burst", "Unique prefixes inserted simultaneously at t=0", burst);
  cmd.AddValue("seed", "ns-3 random seed", seed);
  cmd.AddValue("outputPrefix", "Path prefix for summary CSV/JSON and residence CSV", outputPrefix);
  cmd.Parse(argc, argv);

  if (!(lambda > 0.0) || !std::isfinite(lambda))
    throw std::invalid_argument("lambda must be finite and positive");
  if (!(tc > 0.0) || !std::isfinite(tc))
    throw std::invalid_argument("Tc must be finite and positive");
  if (arrivalCount == 0)
    throw std::invalid_argument("arrivalCount must be positive");
  if (burst > arrivalCount)
    throw std::invalid_argument("burst cannot exceed arrivalCount");
  if (seed == 0)
    throw std::invalid_argument("seed must be positive");

  RngSeedManager::SetSeed(seed);
  RngSeedManager::SetRun(1);
  Ptr<ExponentialRandomVariable> interarrival = CreateObject<ExponentialRandomVariable>();
  interarrival->SetAttribute("Mean", DoubleValue(1.0 / lambda));

  std::vector<double> arrivals(arrivalCount, 0.0);
  double t = 0.0;
  for (uint32_t id = burst; id < arrivalCount; ++id)
  {
    t += interarrival->GetValue();
    arrivals[id] = t;
  }

  TemporaryFibExperiment experiment(tc);
  for (uint32_t id = 0; id < arrivalCount; ++id)
  {
    Simulator::Schedule(Seconds(arrivals[id]), &TemporaryFibExperiment::Arrive, &experiment,
                        static_cast<uint64_t>(id));
  }
  const double lastArrival = *std::max_element(arrivals.begin(), arrivals.end());
  const double horizon = lastArrival + tc;
  Simulator::Stop(Seconds(horizon + 1e-9));
  Simulator::Run();
  experiment.Finish(horizon, arrivalCount);
  Simulator::Destroy();

  const std::vector<Sample>& samples = experiment.Samples();
  std::vector<double> residence;
  residence.reserve(samples.size());
  double residenceSum = 0.0;
  for (std::vector<Sample>::const_iterator it = samples.begin(); it != samples.end(); ++it)
  {
    residence.push_back(it->residence);
    residenceSum += it->residence;
  }
  const double exactExpectedArea = static_cast<double>(arrivalCount) * tc;
  const double areaError = experiment.Area() - exactExpectedArea;
  if (std::fabs(areaError) > std::max(1e-8, exactExpectedArea * 1e-10))
  {
    throw std::runtime_error("temporary FIB occupancy integral does not equal sum of residence times");
  }

  const double timeWeightedMean = experiment.Area() / horizon;
  const double meanResidence = residenceSum / static_cast<double>(arrivalCount);
  const double p50 = Percentile(residence, 0.50);
  const double p95 = Percentile(residence, 0.95);
  const double p99 = Percentile(residence, 0.99);
  const double maxResidence = *std::max_element(residence.begin(), residence.end());
  const double littleLawSteady = lambda * tc;
  const uint32_t poissonArrivals = arrivalCount - burst;
  const double realizedPostBurstRate =
      lastArrival > 0.0 ? static_cast<double>(poissonArrivals) / lastArrival : 0.0;

  const StatefulForwarder::Counters& module = experiment.Module().getCounters();
  const LocalRouteController::Counters& temp =
      experiment.Module().getTemporaryController().getCounters();
  const FlowTableController::Counters& central = experiment.Controller().getCounters();
  const std::string csvPath = outputPrefix + ".summary.csv";
  const std::string jsonPath = outputPrefix + ".summary.json";
  const std::string samplesPath = outputPrefix + ".residence.csv";

  std::ofstream csv(csvPath.c_str());
  csv << "seed,lambda_per_s,arrival_count,burst_count,tc_s,last_arrival_s,horizon_s,high_water,"
         "time_weighted_mean,occupancy_area_s,exact_expected_area_s,area_error_s,mean_residence_s,"
         "p50_residence_s,p95_residence_s,p99_residence_s,max_residence_s,little_law_steady_mean,"
         "realized_post_burst_rate_per_s,trace_updates_accepted,temp_updates,control_packets,"
         "packet_ins,flow_mods_applied,sync_completions,sync_acks_dropped,control_callbacks,flow_"
         "mod_callbacks,sync_complete_callbacks,insert_lookup_checks,delete_lookup_checks,final_"
         "tracked_temporary_fib_size\n";
  csv << std::setprecision(15) << seed << ',' << lambda << ',' << arrivalCount << ',' << burst
      << ',' << tc << ',' << lastArrival << ',' << horizon << ',' << experiment.HighWater() << ','
      << timeWeightedMean << ',' << experiment.Area() << ',' << exactExpectedArea << ','
      << areaError << ',' << meanResidence << ',' << p50 << ',' << p95 << ',' << p99 << ','
      << maxResidence << ',' << littleLawSteady << ',' << realizedPostBurstRate << ','
      << module.traceUpdatesAccepted << ',' << temp.tempUpdates << ',' << temp.controlPackets << ','
      << central.packetIns << ',' << central.flowModsApplied << ',' << temp.syncCompletions << ','
      << central.syncAcksDropped << ',' << experiment.ControlRequestsObserved() << ','
      << experiment.FlowModsObserved() << ',' << experiment.SyncCompleteCallbacks() << ','
      << experiment.InsertLookupChecks() << ',' << experiment.DeleteLookupChecks() << ','
      << experiment.Module().getTemporaryFibSize() << '\n';
  csv.close();

  std::ofstream sampleFile(samplesPath.c_str());
  sampleFile << "seed,prefix_id,arrival_s,completion_s,residence_s\n" << std::setprecision(15);
  for (std::vector<Sample>::const_iterator it = samples.begin(); it != samples.end(); ++it)
  {
    sampleFile << seed << ',' << it->id << ',' << it->arrival << ',' << it->completion << ','
               << it->residence << '\n';
  }
  sampleFile.close();

  std::ostringstream json;
  json << std::setprecision(15) << "{\n  \"schema\": \"statim-temporary-fib-occupancy-v2\",\n"
       << "  \"integration_path\": \"StatefulForwarder::applyTraceUpdate -> "
          "TemporaryForwardingTable -> FlowTableController::submitFlowUpdate -> FLOW_MOD -> "
          "SYNC_ACK -> LocalRouteController::handleControlResponse\",\n"
       << "  \"output_prefix\": \"" << JsonEscape(outputPrefix) << "\",\n"
       << "  \"parameters\": {\"seed\": " << seed << ", \"lambda_per_s\": " << lambda
       << ", \"arrival_count\": " << arrivalCount << ", \"burst_count\": " << burst
       << ", \"tc_s\": " << tc << "},\n"
       << "  \"metrics\": {\"last_arrival_s\": " << lastArrival << ", \"horizon_s\": " << horizon
       << ", \"high_water\": " << experiment.HighWater()
       << ", \"time_weighted_mean\": " << timeWeightedMean
       << ", \"occupancy_area_s\": " << experiment.Area()
       << ", \"exact_expected_area_s\": " << exactExpectedArea
       << ", \"area_error_s\": " << areaError << ", \"mean_residence_s\": " << meanResidence
       << ", \"p50_residence_s\": " << p50 << ", \"p95_residence_s\": " << p95
       << ", \"p99_residence_s\": " << p99 << ", \"max_residence_s\": " << maxResidence
       << ", \"little_law_steady_mean\": " << littleLawSteady
       << ", \"realized_post_burst_rate_per_s\": " << realizedPostBurstRate
       << ", \"final_tracked_temporary_fib_size\": " << experiment.Module().getTemporaryFibSize() << "},\n"
       << "  \"implementation_checks\": {\"trace_updates_accepted\": "
       << module.traceUpdatesAccepted << ", \"temp_updates\": " << temp.tempUpdates
       << ", \"control_packets\": " << temp.controlPackets
       << ", \"packet_ins\": " << central.packetIns
       << ", \"flow_mods_applied\": " << central.flowModsApplied
       << ", \"sync_completions\": " << temp.syncCompletions
       << ", \"sync_acks_dropped\": " << central.syncAcksDropped
       << ", \"control_callbacks\": " << experiment.ControlRequestsObserved()
       << ", \"flow_mod_callbacks\": " << experiment.FlowModsObserved()
       << ", \"sync_complete_callbacks\": " << experiment.SyncCompleteCallbacks()
       << ", \"insert_lookup_checks\": " << experiment.InsertLookupChecks()
       << ", \"delete_lookup_checks\": " << experiment.DeleteLookupChecks() << "},\n"
       << "  \"files\": {\"summary_csv\": \"" << JsonEscape(csvPath) << "\", \"residence_csv\": \""
       << JsonEscape(samplesPath) << "\"},\n"
       << "  \"assumptions\": [\"each arrival is a unique NDN prefix inserted through the real "
          "trace-update path\", \"the initial burst occurs at t=0 and is included in "
          "arrival_count\", \"post-burst inter-arrivals are independent exponential draws\", \"the "
          "central-controller completion delay is deterministic Tc\", \"controller requests "
          "complete independently\", \"temporary storage grows with active prefixes and each "
          "entry is removed after its matching completion notice\"]\n}\n";
  const std::string jsonText = json.str();
  std::ofstream jsonFile(jsonPath.c_str());
  jsonFile << jsonText;
  jsonFile.close();
  std::cout << jsonText;
  return 0;
}
