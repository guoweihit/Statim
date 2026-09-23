# SPDX-License-Identifier: GPL-3.0-or-later
"""Expand named experiment templates into unique executable runs."""
import hashlib
import json
from collections import Counter
from itertools import product

from configuration import DEFAULT_CONFIG, effective_experiments, load_config, validate_config, values

FAULT_CASES = [
    "normal", "duplicate_same_seq", "old_control_late", "old_flowmod_late",
    "control_drop_once", "flowmod_drop_once", "ack_drop_once", "control_drop_all",
    "flowmod_drop_all", "ack_drop_all", "old_ack_late", "same_seq_conflict",
    "shared_route_expiry", "expired_at_install", "expired_before_control",
    "retry_preserves_expiry", "completion_after_expiry", "same_seq_expiry_conflict",
    "refresh_has_new_expiry", "sink_rejection_no_ack",
]
STATE_CASES = [
    "pending owns first stack Interest", "pending duplicate does not add or prune",
    "pending aggregation remembers first nonce", "pending uses ingress insertion time and lifetime",
    "pending consumption does not independently expire", "pending nonpositive lifetime policy",
    "pending consumes all ancestors and deduplicates ports", "pending descendant order and nonce absence",
    "pending nonce must be present on both Interests", "forwarding exact, longest, and root",
    "forwarding expiry boundary and fallback", "forwarding duplicate port does not renew",
    "forwarding insertion prunes expired hops", "forwarding nonpositive lifetimes",
    "forwarding replacement keeps single current hop",
    "both tables support names longer than 32 components", "instances and tables remain independent",
]
PACKET_GROUPS = ["testHeader", "testTraceEncoding", "testMatchFields", "testFlowPipeline",
                 "testSwitch", "testContentStore", "testFibTransmissionCounters",
                 "testDirectReforwardingCounterIsolation", "testKiteRouteLeaseFromInterest",
                 "testRouteLeaseConfigurationValidation", "testConsumerCounters"]

COMMON_OPTIONS = {
    "consumer_cbr_frequency": "consumerCbrFreq", "payload_bytes": "payloadSize",
    "handover_base_s": "hoBase", "handover_jitter_s": "hoJitter",
    "observation_window_s": "windowLen", "stop_s": "stop",
    "refresh_interval_s": "refreshInterval", "trace_lifetime_s": "traceLifeTime",
    "interest_lifetime_s": "interestLifetime", "initial_rtt_estimate_s": "initialRttEstimate",
}
STATIM_OPTIONS = {
    "interest_reforwarding_limit": "interestReforwardingLimit",
    "generation_guard": "generationGuard", "controller_retry_limit": "controllerRetryLimit",
}
VIEWS = {
    "handover": ["handover_delay", "request_satisfaction", "final_forwarding_state"],
    "path": ["handover_delay", "request_satisfaction", "trace_paths", "final_forwarding_state"],
    "occupancy": ["active_state", "residence"],
}


def number(value):
    return str(int(value)) if isinstance(value, bool) else format(value, ".15g")


def arguments(options):
    return ["--{}={}".format(key, options[key]) for key in sorted(options)]


def seeds(settings, profile):
    first = settings["seed_first"]
    return [first] if profile == "quick" else range(first, settings["seed_last"] + 1)


def network_runs(settings, profile):
    scenario = settings["scenario"]
    for system in values(settings["systems"]):
        local = system == "statim"
        axes = [
            values(settings["consumer_retransmissions"]),
            values(settings["interest_reforwarding"]),
            values(settings["temporary_forwarding"]) if local else [None],
            values(settings["controller_delay_s"]) if local else [None],
            values(settings["stateful_delay_us"]) if local else [None],
            values(settings["completion_notice_loss"]) if local and scenario == "handover" else [None],
            values(settings["trace_path_hops"]) if scenario == "path" else [None],
            values(settings["consumer_join"]) if scenario == "path" else [None],
        ]
        for retransmit, reforward, temporary, delay, processing, loss, hops, join in product(*axes):
            parameters = {key: settings[key] for key in COMMON_OPTIONS}
            parameters.update(scenario=scenario, system=system,
                              topology="router-chain" if scenario == "path" else "six-router",
                              consumer_retransmissions=retransmit, interest_reforwarding=reforward)
            options = {destination: number(settings[key]) for key, destination in COMMON_OPTIONS.items()}
            options.update(consumerRetransmissions=number(retransmit),
                           enableInterestReforwarding=number(reforward))
            target = "statim-path" if scenario == "path" else ("statim-grid" if local else "kite-grid")
            expected_keys = ["consumerRetransmissions", "enableInterestReforwarding",
                             "windowLen", "stop", "interestLifetime", "initialRttEstimate"]
            if local:
                parameters.update(temporary_forwarding=temporary, controller_delay_s=delay,
                                  stateful_delay_us=processing)
                options.update(enableTemporaryFib=number(temporary), controllerDelay=number(delay),
                               statefulDelay=number(processing / 1000000))
                expected_keys += ["enableTemporaryFib", "controllerDelay", "statefulDelay"]
                if scenario == "handover":
                    timeout = settings["controller_retry_timeout_s"]
                    if timeout == "auto":
                        timeout = 2 * delay + settings["retry_timeout_margin_s"] if loss else 0
                    parameters.update({key: settings[key] for key in STATIM_OPTIONS})
                    parameters.update(completion_notice_loss=loss, controller_retry_timeout_s=timeout)
                    options.update({destination: number(settings[key]) for key, destination in STATIM_OPTIONS.items()})
                    options.update(controlLossRate=number(loss), controllerRetryTimeout=number(timeout))
                    expected_keys += list(STATIM_OPTIONS.values()) + ["controlLossRate", "controllerRetryTimeout"]
                else:
                    # These settings describe the current path model's fixed control policy.
                    parameters.update(interest_reforwarding_limit=1000, generation_guard=False,
                                      controller_retry_limit=3, controller_retry_timeout_s=0,
                                      completion_notice_loss=0)
            if scenario == "path":
                parameters.update(trace_path_hops=hops, consumer_join=join)
                options.update(tracePathHops=number(hops), consumerJoin=join)
                expected_keys += ["tracePathHops", "consumerJoin", "traceLifeTime", "refreshInterval"]
            expected = {key: options[key] for key in expected_keys}
            expected.update(scenario=target, r=options["consumerCbrFreq"])
            if scenario == "path":
                expected.update(joinIndex=str(hops // 2 if join == "middle" else 0),
                                controllerDelayModel="scheduled-decision-to-effect")
            for seed in seeds(settings, profile):
                yield dict(family="paths" if scenario == "path" else "primary", target=target,
                           argv=arguments(dict(options, run=str(seed))),
                           expected_config=dict(expected, run=str(seed)),
                           parameters=dict(parameters, seed=seed), phase="simulation")


def occupancy_runs(settings, profile, quick):
    for rate, delay, burst in product(values(settings["arrival_rate_per_s"]),
                                     values(settings["controller_delay_s"]), values(settings["initial_burst"])):
        count = settings["arrival_count"]
        if profile == "quick":
            count = min(count, max(quick["occupancy_arrival_count"], burst + 1))
        for seed in seeds(settings, profile):
            parameters = dict(scenario="occupancy", system="statim", seed=seed,
                              arrival_rate_per_s=rate, controller_delay_s=delay,
                              initial_burst=burst, arrival_count=count)
            options = {"lambda": number(rate), "Tc": number(delay), "burst": str(burst),
                       "arrivalCount": str(count), "seed": str(seed)}
            yield dict(family="occupancy", target="temporary-fib-occupancy",
                       argv=arguments(options), expected_config={}, parameters=parameters, phase="simulation")


def expand(config, profile="full", microbenchmark=None):
    if profile not in ("full", "quick"):
        raise ValueError("profile choices: full, quick")
    validate_config(config)
    jobs, identities = [], {}

    def include(job, experiment, views, independent=None):
        identity = json.dumps([job["target"], job["argv"], independent], separators=(",", ":"))
        if identity in identities:
            existing = identities[identity]
            for label, items in (("experiments", [experiment]), ("analysis_views", views)):
                existing[label] = list(dict.fromkeys(existing[label] + items))
            return
        suffix = hashlib.sha256(identity.encode()).hexdigest()[:16]
        name = job["family"] + "-" + suffix
        if any(item["name"] == name for item in jobs):
            raise RuntimeError("Experiment file identifier collision")
        job.update(id=job["family"] + "/" + name, name=name,
                   experiments=[experiment], analysis_views=list(dict.fromkeys(views)))
        if job["family"] == "occupancy":
            job["argv"] += ["--outputPrefix={OUTPUT}/data/raw/occupancy/" + name]
        identities[identity] = job
        jobs.append(job)

    for experiment, settings, extra_views in effective_experiments(config):
        scenario = settings["scenario"]
        runs = (occupancy_runs(settings, profile, config["quick"]) if scenario == "occupancy"
                else network_runs(settings, profile))
        for job in runs:
            include(job, experiment, VIEWS[scenario] + extra_views)

    if config["checks"]["enabled"]:
        for target in config["checks"]["programs"]:
            family = {"statim-controller-faults": "faults", "lpm-consistency": "lookup"}.get(target, "boundary")
            include(dict(family=family, target=target, argv=[], expected_config={},
                         parameters={"check": target}, phase="simulation"), "functional_checks", ["functional_checks"])

    enabled = config["microbenchmark"]["enabled"] if microbenchmark is None else microbenchmark
    if enabled:
        settings = dict(config["microbenchmark"])
        if profile == "quick":
            settings.update({key: value for key, value in config["quick"].items()
                             if key in ("lookup_operations", "trace_data_operations", "repetitions", "batch")})
        for invocation in range(1, settings["invocations"] + 1):
            parameters = {key: settings[key] for key in
                          ("lookup_operations", "trace_data_operations", "repetitions", "batch")}
            parameters["invocation"] = invocation
            include(dict(family="microbenchmark", target="software-microbenchmark",
                         argv=[str(settings[key]) for key in ("lookup_operations", "trace_data_operations", "repetitions", "batch")],
                         expected_config={}, parameters=parameters, phase="cpu-timing"),
                    "software_profile", ["software_operation_cost"], independent=invocation)
    return jobs


def counts(jobs):
    return dict(Counter(job["family"] for job in jobs))
