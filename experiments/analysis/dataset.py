# SPDX-License-Identifier: GPL-3.0-or-later
"""Manifest-driven measurements, statistics and optional raw-data verification."""

from collections import Counter
import csv
import hashlib
import json
import math
from pathlib import Path

from observations import canonical_parameters, mechanism_cases, microbenchmark, occupancy, parameters, simulation
from statistics_core import delay_regressions, finite_number, key, paired_effects, summaries

SIMULATION_TARGETS = {"statim-grid", "kite-grid", "statim-path"}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def write_csv(path, records, empty_fields=("target", "parameters", "metric")):
    """Write observed columns, encoding structured values as JSON."""
    fields = list(dict.fromkeys(name for row in records for name in row)) or list(empty_fields)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for row in records:
            writer.writerow({name: json.dumps(value, ensure_ascii=False, sort_keys=True)
                             if isinstance(value, (list, dict)) else value for name, value in row.items()})


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            value.update(block)
    return value.hexdigest()


def under(root, name):
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError("Recorded output path must be within the run directory: " + str(name))
    return path


def metric_states(issues):
    """Propagate numeric parse status without embedding NaN in saved JSON."""
    states = {}
    for issue in issues:
        if issue.get("value_state") in ("missing", "invalid"):
            field = issue["field"]
            if states.get(field) != "invalid":
                states[field] = issue["value_state"]
    return states


def load_run(root, verify=False):
    """Return measurements, execution errors and mechanism observations."""
    root = Path(root).resolve()
    manifest = read_json(root / "run-manifest.json")
    jobs = manifest["jobs"]
    if len({job["id"] for job in jobs}) != len(jobs):
        raise ValueError("Manifest job identities must be unique")
    records, issues, observations, process_reports, repeatability = [], [], [], [], []
    identity = read_json(root / "run-start.json") if (root / "run-start.json").exists() else {}
    first_runs = {}
    for job in jobs:
        record = dict(job_id=job["id"], family=job["family"], target=job["target"],
                      parameters=parameters(job), experiments=job.get("experiments", [job["family"]]),
                      analysis_views=job.get("analysis_views", []), metrics={})
        try:
            process_path = under(root, "processes/{}/{}.json".format(job["family"], job["name"]))
            process = read_json(process_path)
            if process.get("job_id") != job["id"]:
                raise ValueError("Process and manifest job identities must agree")
            if process.get("exit_code") != 0 or not process.get("passed"):
                raise ValueError("Process reports an execution failure")
            streams = {name: under(root, process[name]) for name in ("stdout", "stderr")}
            if verify:
                for name, path in streams.items():
                    if path.stat().st_size != process[name + "_bytes"] or digest(path) != process[name + "_sha256"]:
                        raise ValueError(name + " differs from its recorded digest")
                recorded_binary = identity.get("binaries", {}).get(job["target"], {}).get("sha256")
                if recorded_binary is None or process.get("binary_sha256") != recorded_binary:
                    raise ValueError("Process binary identity must match the run-start record")
                for item in process.get("generated_files", []):
                    path = under(root, item["path"])
                    if not item.get("exists") or path.stat().st_size != item["bytes"] or digest(path) != item["sha256"]:
                        raise ValueError("Generated file differs from recorded digest: " + item["path"])
            stdout, stderr = [streams[name].read_text(encoding="utf-8") for name in ("stdout", "stderr")]
            observation, problems = [], []
            if job["target"] in SIMULATION_TARGETS:
                record["metrics"], observation, problems = simulation(stderr)
            elif job["target"] == "temporary-fib-occupancy":
                prefix = streams["stdout"].with_suffix("")
                record["metrics"], observation, problems = occupancy(
                    Path(str(prefix) + ".summary.csv").read_text(encoding="utf-8"),
                    Path(str(prefix) + ".summary.json").read_text(encoding="utf-8"),
                    Path(str(prefix) + ".residence.csv") if verify else None)
            elif job["target"] == "software-microbenchmark":
                timing_rows, seen_cells = [], set()
                for index, (dimensions, metrics, timing_issues) in enumerate(microbenchmark(stdout)):
                    signature = key(dimensions)
                    if signature in seen_cells:
                        raise ValueError("Timing cell/repetition identities must be unique within an invocation")
                    seen_cells.add(signature)
                    item = dict(record)
                    item["parameters"] = dict(record["parameters"], **dimensions, invocation=job["id"])
                    item["metrics"] = metrics
                    item["metric_states"] = metric_states(timing_issues)
                    item["measurement_id"] = str(index)
                    timing_rows.append(item)
                    if metrics.get("valid") != 1:
                        problems.append(dict(severity="error", field="valid", reason="invalid_timing_measurement", row=index))
                    problems.extend(dict(issue, row=index) for issue in timing_issues)
                records.extend(timing_rows)
            else:
                observation = mechanism_cases(job["target"], stdout)
            if record["metrics"]:
                record["metric_states"] = metric_states(problems)
                signature = key(dict(target=job["target"], parameters={
                    name: value for name, value in record["parameters"].items() if name != "replicate"}))
                previous = first_runs.get(signature)
                if previous:
                    record["repetition_of"] = previous["job_id"]
                    repeatability.append(dict(job_id=job["id"], reference_job_id=previous["job_id"],
                        stdout_identical=stdout == previous["stdout"], stderr_identical=stderr == previous["stderr"]))
                    if stdout != previous["stdout"] or stderr != previous["stderr"]:
                        problems.append(dict(severity="warning", reason="same_seed_repeat_changed",
                                             reference_job_id=previous["job_id"]))
                else:
                    first_runs[signature] = dict(job_id=job["id"], stdout=stdout, stderr=stderr)
                if job.get("repeat_of") or job.get("repetition_of"):
                    record["repetition_of"] = job.get("repeat_of", job.get("repetition_of"))
                records.append(record)
            observations.extend(dict(job_id=job["id"], **value) for value in observation)
            issues.extend(dict(job_id=job["id"], **problem) for problem in problems)
            process_reports.append(dict(job_id=job["id"], status="loaded"))
        except (OSError, ValueError, KeyError, TypeError) as exc:
            issues.append(dict(job_id=job["id"], severity="error", reason="run_load_error", detail=str(exc)))
            process_reports.append(dict(job_id=job["id"], status="failed"))
    return dict(schema="statim-observations-v2", root=str(root), profile=manifest.get("profile"),
                manifest_jobs=len(jobs), records=records, issues=issues, observations=observations,
                process_reports=process_reports, repeatability=repeatability, raw_verified=verify)


def timing_invocation_means(records):
    """Give each launched invocation equal weight in timing uncertainty summaries."""
    groups, other = {}, []
    for record in records:
        if record["target"] != "software-microbenchmark":
            other.append(record)
            continue
        params = {name: value for name, value in record["parameters"].items() if name != "repetition"}
        groups.setdefault(key(params), []).append(record)
    for signature, members in groups.items():
        record = dict(members[0], parameters=json.loads(signature))
        record["metrics"] = {}
        record["metric_states"] = {}
        for metric in set().union(*(member["metrics"] for member in members)):
            values = [finite_number(member["metrics"].get(metric)) for member in members]
            record["metrics"][metric] = math.fsum(values) / len(values) if all(v is not None for v in values) else None
            if record["metrics"][metric] is None:
                record["metric_states"][metric] = ("invalid" if any(
                    member.get("metric_states", {}).get(metric) == "invalid" for member in members) else "missing")
        other.append(record)
    return other


def analyze(dataset, confidence=0.95):
    records = timing_invocation_means(dataset["records"])
    return dict(summary=summaries(records, confidence), paired=paired_effects(records, confidence),
                regression=delay_regressions(records, confidence))


def compare_datasets(current, historical):
    """Compare metrics matched by all effective parameters and sample identities."""
    def index(records):
        result = {}
        for row in records:
            if row.get("repetition_of") or row["target"] == "software-microbenchmark":
                continue
            signature = key(dict(target=row["target"],
                                 parameters=canonical_parameters(row["target"], row["parameters"])))
            if signature in result:
                raise ValueError("Comparison requires unique parameter/seed identities")
            result[signature] = row
        return result
    left, right = index(current["records"]), index(historical["records"])
    differences = []
    for signature in sorted(left.keys() & right.keys()):
        a, b = left[signature]["metrics"], right[signature]["metrics"]
        for metric in sorted(a.keys() | b.keys()):
            av, bv = a.get(metric), b.get(metric)
            equal = av == bv
            if finite_number(av) is not None and finite_number(bv) is not None:
                equal = math.isclose(float(av), float(bv), rel_tol=1e-9, abs_tol=1e-8)
            if not equal:
                differences.append(dict(parameters=json.loads(signature), metric=metric, current=av, historical=bv))
    matched = len(left.keys() & right.keys())
    status = "no_matching_parameter_cells" if not matched else (
        "differences_observed" if differences else "matched_metrics_equal")
    return dict(status=status, matched_runs=matched, current_only=len(left.keys() - right.keys()),
                historical_only=len(right.keys() - left.keys()), differences=differences,
                timing_comparison="Host timings are available in each dataset's separate summary.")


def load_reference(path):
    path = Path(path).resolve()
    if path.is_file():
        result = read_json(path)
        if "records" not in result:
            raise ValueError("Historical JSON must contain an observations dataset")
        return result
    for root in (path, path.parent):
        if (root / "run-manifest.json").is_file():
            return load_run(root)
    if (path / "records.json").is_file():
        return read_json(path / "records.json")
    raise ValueError("Historical input requires a run manifest or records.json")


def save_analysis(dataset, tables, output, confidence=0.95, historical=None):
    results = analyze(dataset, confidence)
    tables = Path(tables)
    tables.mkdir(parents=True, exist_ok=True)
    write_json(tables / "records.json", dataset)
    for name, rows in results.items():
        empty_fields = {
            "summary": ("target", "parameters", "metric", "n", "mean", "ci_low", "ci_high", "ci_status"),
            "paired": ("target", "parameters", "feature", "metric", "n", "mean", "ci_low", "ci_high", "ci_status"),
            "regression": ("target", "parameters", "metric", "predictor", "seed_fits", "slope", "intercept"),
        }
        write_csv(tables / (name + ".csv"), rows, empty_fields[name])
    write_json(tables / "regression.json", results["regression"])
    write_json(tables / "mechanism-observations.json", dataset["observations"])
    write_json(tables / "issues.json", dataset["issues"])
    counts = Counter(issue["severity"] for issue in dataset["issues"])
    report = dict(schema="statim-analysis-v2", status="data_errors" if counts["error"] else "completed",
        input_root=dataset["root"], confidence=confidence, process_count=dataset["manifest_jobs"],
        loaded_processes=sum(item["status"] == "loaded" for item in dataset["process_reports"]),
        measurement_records=len(dataset["records"]), independent_records=sum(
            not item.get("repetition_of") for item in timing_invocation_means(dataset["records"])),
        errors=counts["error"], warnings=counts["warning"],
        summary_rows=len(results["summary"]), paired_contrasts=len(results["paired"]),
        seed_slope_summaries=len(results["regression"]),
        mechanism_observations=dict(total=len(dataset["observations"]), negative=sum(
            item.get("holds") is False for item in dataset["observations"])),
        repeatability=dataset["repeatability"], raw_verified=dataset["raw_verified"],
        tables=str(tables.resolve()), statistical_units=dict(
            simulation="one independent configured seed per parameter cell",
            regression="mean of within-seed delay-sweep slopes across independent seeds",
            paired="enabled-minus-disabled difference within matched parameters and seed",
            timing="one within-process mean per separately launched invocation and timing cell"))
    if historical is not None:
        report["historical_comparison"] = compare_datasets(dataset, historical)
        report["historical_data_issues"] = historical.get("issues", [])
    write_json(Path(output), report)
    return report
