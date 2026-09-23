# SPDX-License-Identifier: GPL-3.0-or-later
"""Read an annotated base template and named experiment overrides."""
import configparser
import copy
import json
import math
from pathlib import Path

DEFAULT_CONFIG = Path(__file__).with_name("config.ini")
SYSTEMS = ("statim", "nfd-kite")
SCENARIOS = ("handover", "path", "occupancy")
PARAMETERS = {
    "scenario", "systems", "seed_first", "seed_last", "consumer_retransmissions",
    "controller_delay_s", "temporary_forwarding", "interest_reforwarding",
    "consumer_cbr_frequency", "payload_bytes", "handover_base_s", "handover_jitter_s",
    "observation_window_s", "stop_s", "refresh_interval_s", "trace_lifetime_s",
    "interest_lifetime_s", "initial_rtt_estimate_s", "stateful_delay_us",
    "interest_reforwarding_limit", "generation_guard", "controller_retry_limit",
    "completion_notice_loss", "controller_retry_timeout_s", "retry_timeout_margin_s",
    "trace_path_hops", "consumer_join", "arrival_rate_per_s", "arrival_count", "initial_burst",
}
EXPERIMENT_OPTIONS = {"enabled", "analysis_views"}
SECTION_KEYS = {
    "checks": {"enabled", "programs"},
    "microbenchmark": {"enabled", "lookup_operations", "trace_data_operations", "repetitions", "batch", "invocations"},
    "quick": {"occupancy_arrival_count", "lookup_operations", "trace_data_operations", "repetitions", "batch"},
}
CHECK_PROGRAMS = {"state-table-tests", "statim-packet-pipeline-tests", "statim-controller-faults", "lpm-consistency"}
ARRAY_KEYS = {"systems", "consumer_retransmissions", "controller_delay_s", "temporary_forwarding",
              "interest_reforwarding", "stateful_delay_us", "completion_notice_loss",
              "trace_path_hops", "consumer_join", "arrival_rate_per_s", "initial_burst"}
BOOLEAN_KEYS = {"consumer_retransmissions", "temporary_forwarding", "interest_reforwarding", "generation_guard"}
INTEGER_KEYS = {"seed_first", "seed_last", "payload_bytes", "interest_reforwarding_limit",
                "controller_retry_limit", "trace_path_hops", "arrival_count", "initial_burst"}
POSITIVE_KEYS = {"consumer_cbr_frequency", "observation_window_s", "stop_s", "refresh_interval_s",
                 "trace_lifetime_s", "interest_lifetime_s", "initial_rtt_estimate_s", "arrival_rate_per_s", "arrival_count"}
SELECTION_KEYS = {"scenario", "systems", "seed_first", "seed_last"}
MOBILITY_KEYS = {
    "consumer_retransmissions", "interest_reforwarding", "temporary_forwarding",
    "controller_delay_s", "consumer_cbr_frequency", "payload_bytes", "handover_base_s",
    "handover_jitter_s", "observation_window_s", "stop_s", "refresh_interval_s",
    "trace_lifetime_s", "interest_lifetime_s", "initial_rtt_estimate_s", "stateful_delay_us",
}
CONTROL_KEYS = {"interest_reforwarding_limit", "generation_guard", "controller_retry_limit",
                "completion_notice_loss", "controller_retry_timeout_s", "retry_timeout_margin_s"}
SCENARIO_KEYS = {
    "handover": SELECTION_KEYS | MOBILITY_KEYS | CONTROL_KEYS,
    "path": SELECTION_KEYS | MOBILITY_KEYS | {"trace_path_hops", "consumer_join"},
    "occupancy": SELECTION_KEYS | {"controller_delay_s", "arrival_rate_per_s", "arrival_count", "initial_burst"},
}
STATIM_ONLY_KEYS = CONTROL_KEYS | {"temporary_forwarding", "controller_delay_s", "stateful_delay_us"}


def number(value, key, positive=False, integer=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(key + " requires a finite number")
    if (integer and not isinstance(value, int)) or value < 0 or (positive and value == 0):
        raise ValueError(key + " has an invalid value or numeric type")


def values(value):
    return value if isinstance(value, list) else [value]


def validate_parameters(parameters, label):
    for key, value in parameters.items():
        if key not in PARAMETERS:
            raise ValueError(label + ": unknown parameter " + key)
        entries = values(value)
        if key in ARRAY_KEYS:
            if not entries or len({json.dumps(item, sort_keys=True) for item in entries}) != len(entries):
                raise ValueError(label + "." + key + " requires distinct, nonempty values")
        elif isinstance(value, list):
            raise ValueError(label + "." + key + " requires one value")
        for item in entries:
            if key == "scenario":
                if item not in SCENARIOS:
                    raise ValueError("scenario choices: " + ", ".join(SCENARIOS))
            elif key == "systems":
                if not isinstance(item, str) or item not in SYSTEMS:
                    raise ValueError("systems choices: " + ", ".join(SYSTEMS))
            elif key == "consumer_join":
                if item not in ("rendezvous", "middle"):
                    raise ValueError("consumer_join choices: rendezvous, middle")
            elif key == "controller_retry_timeout_s" and item == "auto":
                pass
            elif key in BOOLEAN_KEYS:
                if not isinstance(item, bool):
                    raise ValueError(label + "." + key + " requires true or false")
            else:
                number(item, key, positive=key in POSITIVE_KEYS, integer=key in INTEGER_KEYS)
                if key == "completion_notice_loss" and item > 1:
                    raise ValueError("completion_notice_loss requires 0 <= probability <= 1")
                if key == "trace_path_hops" and item < 2:
                    raise ValueError("trace_path_hops requires at least two router links")
                if key in INTEGER_KEYS and item > 4294967295:
                    raise ValueError(key + " must fit the simulator's 32-bit unsigned parameter")
                if key in ("payload_bytes", "interest_reforwarding_limit", "controller_retry_limit") and item > 2147483647:
                    raise ValueError(key + " must fit the scenario's signed integer parameter")
                if key in ("interest_lifetime_s", "trace_lifetime_s"):
                    milliseconds = item * 1000
                    if item < 0.001 or not math.isclose(milliseconds, round(milliseconds), rel_tol=0, abs_tol=1e-7):
                        raise ValueError(key + " requires whole milliseconds (seconds >= 0.001)")
    if not 1 <= parameters["seed_first"] <= parameters["seed_last"] <= 4294967295:
        raise ValueError(label + ": seed range requires 1 <= first <= last <= 4294967295")
    scenario = parameters["scenario"]
    if scenario in ("handover", "path"):
        if parameters["handover_base_s"] < 1 or parameters["handover_base_s"] + parameters["handover_jitter_s"] + parameters["observation_window_s"] >= parameters["stop_s"] - 2:
            raise ValueError(label + ": handover and observation window must fit inside 1 .. stop_s-2")
        if scenario == "path":
            if values(parameters["systems"]) != ["statim"]:
                raise ValueError("The path scenario uses the Statim forwarding model")
            if min(parameters["trace_lifetime_s"], parameters["refresh_interval_s"]) <= parameters["stop_s"]:
                raise ValueError("Path isolation requires route lifetime and refresh interval greater than stop_s")
    if scenario == "occupancy":
        if values(parameters["systems"]) != ["statim"]:
            raise ValueError("The occupancy scenario uses the Statim state model")
        if any(delay <= 0 for delay in values(parameters["controller_delay_s"])):
            raise ValueError("Occupancy completion delays must be positive")
        if any(burst > parameters["arrival_count"] for burst in values(parameters["initial_burst"])):
            raise ValueError("An initial burst fits inside arrival_count")


def load_config(path=DEFAULT_CONFIG):
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    try:
        with Path(path).open(encoding="utf-8-sig") as stream:
            parser.read_file(stream)
    except configparser.Error as exc:
        raise ValueError(str(exc))
    result = {"experiments": {}}
    if parser.defaults():
        raise ValueError("Use the explicit [defaults] section")
    def reject_constant(value):
        raise ValueError("Use a finite JSON value: " + value)
    for section in parser.sections():
        if section == "defaults":
            allowed = PARAMETERS
        elif section.startswith("experiment.") and section[len("experiment."):]:
            allowed = PARAMETERS | EXPERIMENT_OPTIONS
        elif section in SECTION_KEYS:
            allowed = SECTION_KEYS[section]
        else:
            raise ValueError("Unknown configuration section: " + section)
        payload = {}
        for key, raw in parser[section].items():
            if key not in allowed:
                raise ValueError("[{}] unknown key {}".format(section, key))
            try:
                payload[key] = json.loads(raw, parse_constant=reject_constant)
            except ValueError as exc:
                raise ValueError("[{}] {}: {}".format(section, key, exc))
        if section.startswith("experiment."):
            result["experiments"][section.split(".", 1)[1]] = payload
        else:
            result[section] = payload
    if set(result) != {"defaults", "experiments", "checks", "microbenchmark", "quick"}:
        raise ValueError("Configuration requires defaults, named experiments, checks, microbenchmark, quick")
    if set(result["defaults"]) != PARAMETERS:
        raise ValueError("The defaults template must define: " + ", ".join(sorted(PARAMETERS)))
    for section, required in SECTION_KEYS.items():
        if set(result[section]) != required:
            raise ValueError("[{}] requires {}".format(section, ", ".join(sorted(required))))
    validate_config(result)
    return result


def effective_experiments(config):
    for name, overrides in config["experiments"].items():
        if not isinstance(overrides.get("enabled", True), bool):
            raise ValueError(name + ".enabled requires true or false")
        view = overrides.get("analysis_views", [])
        if not isinstance(view, list) or any(not isinstance(item, str) for item in view):
            raise ValueError(name + ".analysis_views requires a list of labels")
        effective = copy.deepcopy(config["defaults"])
        effective.update({key: value for key, value in overrides.items() if key not in EXPERIMENT_OPTIONS})
        validate_parameters(effective, name)
        allowed = SCENARIO_KEYS[effective["scenario"]]
        if values(effective["systems"]) == ["nfd-kite"]:
            allowed = allowed - STATIM_ONLY_KEYS
        unused = set(overrides) - EXPERIMENT_OPTIONS - allowed
        if unused:
            raise ValueError(name + ": parameters outside this scenario: " + ", ".join(sorted(unused)))
        if overrides.get("enabled", True):
            yield name, effective, list(view)


def validate_config(config):
    if set(config["defaults"]) != PARAMETERS:
        raise ValueError("The defaults template must define every parameter")
    validate_parameters(config["defaults"], "defaults")
    list(effective_experiments(config))
    for section in ("checks", "microbenchmark"):
        if not isinstance(config[section]["enabled"], bool):
            raise ValueError(section + ".enabled requires true or false")
    programs = config["checks"]["programs"]
    if not isinstance(programs, list) or any(not isinstance(item, str) or item not in CHECK_PROGRAMS for item in programs) or len(programs) != len(set(programs)):
        raise ValueError("checks.programs selects distinct supplied check programs")
    for section in ("microbenchmark", "quick"):
        for key, value in config[section].items():
            if key != "enabled":
                number(value, section + "." + key, positive=True, integer=True)
        if config[section]["batch"] > min(config[section]["lookup_operations"], config[section]["trace_data_operations"]):
            raise ValueError(section + ": batch size fits both operation counts")
