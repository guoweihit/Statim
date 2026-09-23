# SPDX-License-Identifier: GPL-3.0-or-later
"""Read recorded measurements using manifest identities and log field names."""

from collections import defaultdict
import csv
import io
import json
import math
import re

from statistics_core import finite_number


OPTION_NAMES = {
    "consumerCbrFreq": "consumer_cbr_frequency", "payloadSize": "payload_bytes",
    "hoBase": "handover_base_s", "hoJitter": "handover_jitter_s",
    "windowLen": "observation_window_s", "stop": "stop_s",
    "refreshInterval": "refresh_interval_s", "traceLifeTime": "trace_lifetime_s",
    "retxTime": "interest_lifetime_s", "interestLifetime": "interest_lifetime_s",
    "rttInitialEstimate": "initial_rtt_estimate_s", "initialRttEstimate": "initial_rtt_estimate_s",
    "interestReforwardingLimit": "interest_reforwarding_limit",
    "generationGuard": "generation_guard", "controllerRetryLimit": "controller_retry_limit",
    "enableInterestReforwarding": "interest_reforwarding",
    "enableTemporaryFib": "temporary_forwarding",
    "consumerRetransmissions": "consumer_retransmissions",
    "statefulDelay": "stateful_delay_s", "controllerDelay": "controller_delay_s",
    "controlLossRate": "completion_notice_loss",
    "controllerRetryTimeout": "controller_retry_timeout_s",
    "tracePathHops": "trace_path_hops", "consumerJoin": "consumer_join",
    "lambda": "arrival_rate_per_s", "Tc": "controller_delay_s",
    "arrivalCount": "arrival_count", "burst": "initial_burst",
}
BOOLEAN_FIELDS = {"temporary_forwarding", "interest_reforwarding", "consumer_retransmissions",
                  "generation_guard"}


def kv(line):
    return dict(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", line))


def scalar(value):
    if isinstance(value, (bool, int, float)) or value is None:
        return value
    if value in ("true", "false"):
        return value == "true"
    number = finite_number(value)
    return number if number is not None else value


def numeric(value, field, issues, severity="error"):
    """Read a required metric while retaining absent versus invalid provenance."""
    result = finite_number(value)
    if result is None:
        issues.append(dict(severity=severity, field=field, reason="missing_or_nonfinite_metric",
                           value_state="missing" if value is None or value == "" else "invalid",
                           recorded_value=None if value is None else str(value)))
    return result


def parameters(job):
    """Use complete effective manifest parameters; read argv for earlier datasets."""
    if job.get("parameters"):
        result = dict(job["parameters"])
    else:
        result = {}
        for arg in job.get("argv", []):
            if arg.startswith("--") and "=" in arg:
                name, value = arg[2:].split("=", 1)
                if name != "outputPrefix":
                    result[OPTION_NAMES.get(name, name)] = scalar(value)
        if any(arg.startswith("--retxTime=") for arg in job.get("argv", [])):
            result.setdefault("initial_rtt_estimate_s", result["interest_lifetime_s"])
        if "stateful_delay_s" in result:
            result["stateful_delay_us"] = result.pop("stateful_delay_s") * 1000000
        if job["target"] == "software-microbenchmark":
            positional = job.get("argv", [])
            if len(positional) == 4:
                result.update(zip(("lookup_operations", "trace_data_operations", "repetitions", "batch"),
                                  map(scalar, positional)))
    return canonical_parameters(job["target"], result)


def canonical_parameters(target, recorded):
    """Give recorded versions a common vocabulary for their effective settings.

    The network scenarios use their ``run`` argument as the ns-3 RNG run index.
    The annotated configuration calls that independent sample identifier a seed.
    Topology and fixed path-control values follow the corresponding scenarios.
    Explicit recorded values take precedence over these fixed model settings.
    """
    result = dict(recorded)
    if "completion_notice_loss_probability" in result:
        value = result.pop("completion_notice_loss_probability")
        if "completion_notice_loss" in result and result["completion_notice_loss"] != value:
            raise ValueError("Recorded completion-notice loss settings disagree")
        result["completion_notice_loss"] = value
    if target in ("statim-grid", "statim-path", "kite-grid"):
        if "run" in result:
            value = result.pop("run")
            if "seed" in result and result["seed"] != value:
                raise ValueError("Recorded seed and scenario RNG run index disagree")
            result["seed"] = value
        path = target == "statim-path"
        result.setdefault("system", "nfd-kite" if target == "kite-grid" else "statim")
        result.setdefault("scenario", "path" if path else "handover")
        result.setdefault("topology", "router-chain" if path else "six-router")
        if path:
            # statim-path sets a 1000-Interest budget; its helper supplies the
            # remaining fixed control policy in both recorded source versions.
            for name, value in dict(interest_reforwarding_limit=1000, generation_guard=False,
                                    controller_retry_limit=3, controller_retry_timeout_s=0,
                                    completion_notice_loss=0).items():
                result.setdefault(name, value)
    elif target == "temporary-fib-occupancy":
        result.setdefault("scenario", "occupancy")
        result.setdefault("system", "statim")
    elif target in ("statim-controller-faults", "lpm-consistency", "state-table-tests",
                    "statim-packet-pipeline-tests"):
        result.setdefault("check", target)
    for name in BOOLEAN_FIELDS & result.keys():
        if result[name] in (True, False, 0, 1):
            result[name] = bool(result[name])
    return result


def csv_rows(text, delimiter=","):
    lines = [line for line in text.splitlines()
             if line.strip() and not line.startswith(("#", "SUMMARY"))]
    return list(csv.DictReader(io.StringIO("\n".join(lines)), delimiter=delimiter))


def tagged(text):
    result = defaultdict(list)
    for line in text.splitlines():
        match = re.match(r"^\[([A-Z_]+)\]", line)
        if match:
            result[match[1]].append(kv(line))
    return result


def fib_counter_metrics(rows, issues, severity="warning"):
    """Keep per-egress transmission counts distinct from historical counters.

    Historical temporaryFibForwards counts matching Interests, whereas
    flowTableFibForwards counts all normal output-port sends. Their units and
    overlapping populations are retained in explicitly legacy metric names.
    """
    schemas = (
        {"temporary_fib_interest_transmissions": "temporaryFibInterestTransmissions",
         "flow_table_fib_interest_transmissions": "flowTableFibInterestTransmissions"},
        {"legacy_temporary_fib_matches": "temporaryFibForwards",
         "legacy_interest_transmissions": "flowTableFibForwards"},
    )
    metrics = {}
    for mapping in schemas:
        if any(field in row for field in mapping.values() for row in rows):
            for name, field in mapping.items():
                values = [numeric(row.get(field), name, issues, severity) for row in rows]
                metrics[name] = sum(values) if all(value is not None for value in values) else None
    if rows and not metrics:
        issues.append(dict(severity=severity, field="fib_counters",
                           reason="missing_fib_counter_schema"))
    return metrics


def simulation(text):
    tags = tagged(text)
    issues, observations = [], []
    configs = tags["CONFIG"]
    if len(configs) != 1:
        raise ValueError("Expected one CONFIG record; observed {}".format(len(configs)))
    config = configs[0]
    handover = finite_number(config.get("t_ho"))
    if handover is None:
        raise ValueError("CONFIG requires a finite t_ho field")
    metrics = {"handover_delay_ms": None}
    candidates = []
    for row in tags["HO_DELAY"]:
        sent = finite_number(row.get("TI_sent"))
        if sent is not None and sent >= handover - 1e-6:
            candidates.append((sent, row.get("delay", "").removesuffix("ms")))
    if candidates:
        raw_value = min(candidates, key=lambda item: item[0])[1]
        metrics["handover_delay_ms"] = numeric(raw_value, "handover_delay_ms", issues)
    else:
        issues.append(dict(severity="warning", field="handover_delay_ms",
                           reason="handover_completion_unobserved",
                           detail="The observation window contains no recorded handover completion."))
    for tag, mapping in {
        "WINDOW_LOSS": {"window_sent": "sent_in_window", "window_unsatisfied": "never_satisfied",
                        "window_unsatisfied_pct": "frac_pct"},
        "RUN_LOSS": {"unique_sent": "unique_sent", "run_unsatisfied": "never_satisfied_total"},
    }.items():
        if len(tags[tag]) != 1:
            raise ValueError("Expected one {} record; observed {}".format(tag, len(tags[tag])))
        for name, field in mapping.items():
            metrics[name] = numeric(tags[tag][0].get(field), name, issues)
    if metrics["window_sent"] is not None and metrics["window_unsatisfied"] is not None:
        denominator = metrics["window_sent"]
        numerator = metrics["window_unsatisfied"]
        if denominator < 0 or numerator < 0 or numerator > denominator:
            issues.append(dict(severity="error", field="window_unsatisfied", reason="invalid_count_bounds"))
        elif denominator == 0:
            metrics["window_unsatisfied_pct"] = None
            issues.append(dict(severity="warning", field="window_unsatisfied_pct", reason="empty_observation_window"))
    counters = {
        "control_packets": "controlPkts", "flow_modifications": "flowMods",
        "completed_synchronizations": "syncCompletions", "reforwarded_interests": "pullsSent",
        "controller_retries": "controllerRetries", "temporary_route_withdrawals": "temporaryRouteWithdrawals",
        "dropped_completion_notices": "acksDropped",
    }
    if tags["STATIM_COUNTERS"]:
        for name, field in counters.items():
            values = [numeric(row.get(field), name, issues, "warning") for row in tags["STATIM_COUNTERS"]]
            metrics[name] = sum(values) if all(value is not None for value in values) else None
        metrics.update(fib_counter_metrics(tags["STATIM_COUNTERS"], issues))
    if tags["TEMPORARY_FIB_PEAK"]:
        peaks = [numeric(row.get("peak"), "temporary_fib_peak", issues, "warning") for row in tags["TEMPORARY_FIB_PEAK"]]
        metrics["temporary_fib_peak"] = max(peaks) if all(value is not None for value in peaks) else None
    if tags["FIB_FINAL"]:
        states = [finite_number(row.get("ok")) for row in tags["FIB_FINAL"]]
        metrics["final_fib_checked"] = len(states)
        metrics["final_fib_consistent"] = sum(value == 1 for value in states)
        observations.append(dict(name="final_fib_consistency", holds=all(value == 1 for value in states)))
    if config.get("scenario") == "statim-path":
        path_metrics, path_observations = path_observations_from_tags(tags, handover, issues)
        metrics.update(path_metrics)
        observations.extend(path_observations)
    return metrics, observations, issues


def path_observations_from_tags(tags, handover, issues):
    config = tags["CONFIG"][0]
    paths = {}
    labels = {row["nodeId"]: row.get("label", row["nodeId"]) for row in tags["PATH_NODEMAP"]}
    observed = []
    for kind in ("TI", "TD"):
        events = [(finite_number(row.get("t")), row.get("nodeId")) for row in tags["TRACE_" + kind]]
        events = sorted((time, node) for time, node in events if time is not None and time >= handover - 1e-6)
        paths[kind] = events
        nodes = list(dict.fromkeys(node for _, node in events))
        observed.append(dict(name="trace_{}_router_path".format(kind.lower()),
                             nodes=[labels.get(node, node) for node in nodes]))
    metrics = {"trace_round_trip_ms": None, "handover_to_last_trace_data_ms": None,
               "first_steer_after_handover_ms": None}
    if paths["TI"] and paths["TD"]:
        metrics["trace_round_trip_ms"] = (paths["TD"][-1][0] - paths["TI"][0][0]) * 1000
        metrics["handover_to_last_trace_data_ms"] = (paths["TD"][-1][0] - handover) * 1000
    steer = [(finite_number(row.get("t")), row) for row in tags["STEER_FIRST"]]
    steer = [(time, row) for time, row in steer if time is not None and time >= handover - 1e-6]
    if steer:
        time, row = min(steer, key=lambda item: item[0])
        metrics["first_steer_after_handover_ms"] = (time - handover) * 1000
        observed.append(dict(name="first_steer", node=labels.get(row.get("nodeId")), source=row.get("source")))
    for tag in ("PATH_TOTAL", "PATH_FIB_TOTAL"):
        if len(tags[tag]) != 1:
            raise ValueError("Expected one {} record".format(tag))
    totals = tags["PATH_TOTAL"][0]
    for name, field in {"control_packets": "controls", "flow_modifications": "flowMods",
                        "packet_in_messages": "packetIns", "temporary_updates": "tempUpdates"}.items():
        metrics[name] = numeric(totals.get(field), name, issues)
    metrics.update(fib_counter_metrics([totals], issues, "error"))
    hops = finite_number(config.get("tracePathHops"))
    for kind in ("TI", "TD"):
        measured = max(len({node for _, node in paths[kind]}) - 1, 0)
        metrics["trace_{}_observed_hops".format(kind.lower())] = measured
        counted = finite_number(totals.get("trace{}BackboneHops".format(kind.title())))
        observed.append(dict(name="trace_{}_path_matches_configuration".format(kind.lower()),
                             holds=measured == counted == hops, configured=hops,
                             observed=measured, counted=counted))
    observed.append(dict(name="final_flow_path_consistency",
                         holds=finite_number(tags["PATH_FIB_TOTAL"][0].get("allOk")) == 1))
    return metrics, observed


def occupancy(summary_text, json_text, residence_path=None):
    rows = csv_rows(summary_text)
    if len(rows) != 1:
        raise ValueError("Expected one occupancy summary row")
    row = rows[0]
    issues = []
    metrics = {name: numeric(value, name, issues) for name, value in row.items()
               if name not in {"seed", "lambda_per_s", "tc_s", "burst_count", "arrival_count"}}
    payload = json.loads(json_text)
    for section in ("parameters", "metrics", "implementation_checks"):
        for name, value in payload[section].items():
            observed = finite_number(row.get(name))
            expected = finite_number(value)
            if observed is None or expected is None or not math.isclose(observed, expected, rel_tol=1e-10, abs_tol=1e-8):
                issues.append(dict(severity="error", field=name, reason="occupancy_csv_json_mismatch"))
    count = finite_number(row.get("arrival_count"))
    tc = finite_number(row.get("tc_s"))
    rate = finite_number(row.get("lambda_per_s"))
    if count is None or tc is None or rate is None or count < 1 or tc <= 0 or rate <= 0:
        raise ValueError("Occupancy parameters require positive arrivals, delay and rate")
    observations = [dict(name="occupancy_area_identity", holds=(
        math.isclose(metrics["occupancy_area_s"], count * tc, rel_tol=1e-8, abs_tol=1e-7)
        if metrics.get("occupancy_area_s") is not None else None)),
        dict(name="mean_residence_matches_completion_delay", holds=(
            math.isclose(metrics["mean_residence_s"], tc, rel_tol=1e-8, abs_tol=1e-8)
            if metrics.get("mean_residence_s") is not None else None))]
    metrics["relative_deviation_from_stationary_occupancy"] = (
        (metrics["time_weighted_mean"] - rate * tc) / (rate * tc)
        if metrics.get("time_weighted_mean") is not None else None)
    if metrics["relative_deviation_from_stationary_occupancy"] is None:
        numeric(row.get("time_weighted_mean"), "relative_deviation_from_stationary_occupancy", issues)
    if residence_path is not None:
        observations.append(verify_residence(residence_path, row))
    return metrics, observations, issues


def verify_residence(path, summary):
    """Recompute trajectory counts, residence integral and peak for the actual run."""
    ids, events, durations = set(), [], []
    tc = float(summary["tc_s"])
    with path.open(encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            a, b, duration = [finite_number(row.get(name)) for name in ("arrival_s", "completion_s", "residence_s")]
            if any(value is None for value in (a, b, duration)):
                raise ValueError("Residence sample requires finite arrival/completion/residence")
            if row["prefix_id"] in ids or row["seed"] != summary["seed"]:
                raise ValueError("Residence sample identities must be unique and match the run seed")
            ids.add(row["prefix_id"])
            if b < a or not math.isclose(b - a, duration, abs_tol=1e-8, rel_tol=1e-8):
                raise ValueError("Residence duration must match completion minus arrival")
            durations.append(duration)
            # All arrivals are scheduled before generated completion callbacks.
            events.extend(((a, 0, 1), (b, 1, -1)))
    active, peak = 0, 0
    for _, _, delta in sorted(events):
        active += delta
        peak = max(peak, active)
    area = math.fsum(durations)
    checks = dict(samples_match_arrival_count=len(ids) == int(summary["arrival_count"]),
                  final_occupancy_empty=active == 0,
                  peak_matches_summary=peak == int(summary["high_water"]),
                  area_matches_summary=math.isclose(area, float(summary["occupancy_area_s"]), rel_tol=1e-8, abs_tol=1e-6),
                  mean_matches_summary=math.isclose(area / float(summary["horizon_s"]),
                      float(summary["time_weighted_mean"]), rel_tol=1e-8, abs_tol=1e-7))
    if not all(checks.values()):
        raise ValueError("Residence reconstruction mismatch: " + json.dumps(checks))
    return dict(name="raw_residence_reconstruction", samples=len(ids), checks=checks,
                deterministic_delay_observed=all(math.isclose(value, tc, rel_tol=1e-8, abs_tol=1e-8) for value in durations))


def microbenchmark(text):
    """Retain each recorded timing repetition with its full cell identity."""
    result = []
    for row in csv_rows(text):
        dimensions = ("benchmark", "path", "outcome", "table_size", "bookkeeping",
                      "temporary_fib_write", "control_dispatch")
        parameters = {name: scalar(row[name]) for name in dimensions}
        parameters["repetition"] = scalar(row["rep"])
        issues = []
        metrics = {name: numeric(value, name, issues) for name, value in row.items()
                   if name not in set(dimensions) | {"rep"}}
        result.append((parameters, metrics, issues))
    if not result:
        raise ValueError("Microbenchmark output requires measurement rows")
    return result


def mechanism_cases(target, stdout):
    """Expose constructive cases and their reported results as observations."""
    if target in ("statim-controller-faults", "lpm-consistency"):
        rows = csv_rows(stdout, "\t" if target == "lpm-consistency" else ",")
        if not rows:
            raise ValueError("Case output requires records")
        return [dict(name=row.get("case", "case"), holds=row.get("pass") in ("1", "true"),
                     details=row) for row in rows]
    if target == "state-table-tests":
        matches = re.findall(r"^(\d+)/(\d+) state-table cases passed$", stdout, re.MULTILINE)
        if len(matches) != 1:
            raise ValueError("State-table output requires a case summary")
        passed, total = map(int, matches[0])
        return [dict(name="state_table_cases", holds=passed == total, passed=passed, total=total)]
    if target == "statim-packet-pipeline-tests":
        matches = re.findall(r"^packet-pipeline: PASS \((\d+) checks\)$", stdout, re.MULTILINE)
        if len(matches) != 1:
            raise ValueError("Pipeline output requires a check summary")
        return [dict(name="packet_pipeline_cases", holds=True, checks=int(matches[0]))]
    raise ValueError("Analysis parser is required for target " + target)
