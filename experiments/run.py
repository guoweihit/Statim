#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the configured Statim experiments after the normal ns-3 waf build."""
import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.dont_write_bytecode = True
from matrix import DEFAULT_CONFIG, FAULT_CASES, PACKET_GROUPS, STATE_CASES, counts, expand, load_config

ROOT = Path(__file__).resolve().parents[1]
TOPOLOGY = Path("src/ndnSIM/statim/scenarios/common/grid-topology.txt")


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def is_under(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def find_binaries(root, targets):
    directory = root / "build/src/ndnSIM/statim"
    binaries = {}
    for target in sorted(targets):
        exact = directory / ("ns3-dev-" + target + "-optimized")
        matches = [exact] if exact.is_file() else [
            path for path in directory.glob("*")
            if path.is_file() and (path.name == target or
                re.fullmatch(r"ns3[^/]*-" + re.escape(target) + r"(?:-(?:optimized|debug|release))?", path.name))
        ]
        matches = [path for path in matches if os.access(str(path), os.X_OK)]
        if len(matches) != 1:
            raise RuntimeError("Expected one built executable for {} in {}; run the root waf build first. Found: {}".format(
                target, directory, [path.name for path in matches]))
        binaries[target] = matches[0].resolve()
    return binaries


def runtime_environment(root):
    env = dict(os.environ)
    build = root / "build"
    library_dirs = {str(build.resolve())}
    library_dirs.update(str(path.parent.resolve()) for path in build.rglob("*.so*"))
    previous = env.get("LD_LIBRARY_PATH")
    env["LD_LIBRARY_PATH"] = os.pathsep.join(sorted(library_dirs) + ([previous] if previous else []))
    return env


def check_output(job, stdout, stderr):
    errors = []
    if job["target"] == "state-table-tests":
        observed = [line[5:] for line in stdout.splitlines() if line.startswith("PASS ")]
        if len(observed) != len(STATE_CASES) or set(observed) != set(STATE_CASES):
            errors.append("Expected all 17 state-table cases to pass")
    if job["target"] == "statim-packet-pipeline-tests":
        matches = re.findall(r"^packet-pipeline: PASS \((\d+) checks\)$", stdout, re.MULTILINE)
        if len(matches) != 1 or int(matches[0]) <= 0:
            errors.append("Packet-pipeline test did not report successful checks")
    if job["expected_config"]:
        configs = [dict(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", line))
                   for line in stderr.splitlines() if line.startswith("[CONFIG]")]
        if len(configs) != 1:
            errors.append("Expected exactly one CONFIG record; found {}".format(len(configs)))
        else:
            for key, expected in job["expected_config"].items():
                observed = configs[0].get(key)
                try:
                    equal = math.isfinite(float(observed)) and abs(float(expected) - float(observed)) <= 1e-9
                except (TypeError, ValueError):
                    equal = expected == observed
                if not equal:
                    errors.append("{}: expected {}, observed {}".format(key, expected, observed))
    return errors


def run(args):
    config = load_config(args.config)
    jobs = expand(config, args.profile, microbenchmark=getattr(args, "microbenchmark", None))
    if not jobs:
        raise ValueError("Select at least one experiment, functional check, or software profile")
    output = args.output.resolve()
    if is_under(output, ROOT):
        raise ValueError("Choose an output directory outside the ns-3 source tree")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError("Output must be new or empty; choose another run directory")
    if args.jobs < 1 or not math.isfinite(args.timeout) or args.timeout <= 0:
        raise ValueError("--jobs and --timeout must be positive")
    binaries = find_binaries(ROOT, {job["target"] for job in jobs})
    if not (ROOT / TOPOLOGY).is_file():
        raise RuntimeError("Missing bundled topology: " + str(ROOT / TOPOLOGY))
    env = runtime_environment(ROOT)
    allowed = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    cpu = args.cpu if args.cpu is not None else (allowed[0] if allowed else None)
    if args.cpu is not None and (args.cpu < 0 or (allowed and args.cpu not in allowed)):
        raise ValueError("Requested CPU is outside this process's allowed affinity: " + str(allowed))
    taskset = shutil.which("taskset")
    has_timing = any(job["phase"] == "cpu-timing" for job in jobs)
    if has_timing and cpu is not None and not taskset:
        raise RuntimeError("taskset is needed to pin the CPU microbenchmark (provided by util-linux)")
    if has_timing and cpu is None:
        print("CPU affinity is unavailable on this platform; microbenchmark will be unpinned", file=sys.stderr)
    output.mkdir(parents=True, exist_ok=True)
    work = output / "work"
    topology = work / TOPOLOGY
    topology.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / TOPOLOGY, topology)
    for family in counts(jobs):
        (output / "data/raw" / family).mkdir(parents=True, exist_ok=True)
    identities = {target: {"path": str(path), "sha256": sha(path)} for target, path in binaries.items()}
    started = utc()
    manifest = {"schema": "statim-experiment-plan-v2", "profile": args.profile,
                "config": config, "counts": counts(jobs), "jobs": jobs,
                "experiment_counts": {name: sum(name in job["experiments"] for job in jobs)
                                      for name in sorted({name for job in jobs for name in job["experiments"]})},
                "process_count": len(jobs), "fault_cases": FAULT_CASES,
                "boundary": {"state_table_cases": STATE_CASES, "packet_groups": PACKET_GROUPS}}
    write_json(output / "run-manifest.json", manifest)
    write_json(output / "run-start.json", {"started_utc": started, "platform": platform.platform(),
               "binaries": identities, "microbenchmark_cpu": cpu if has_timing else None, "cpu_affinity_allowed": allowed,
               "simulation_concurrency": args.jobs, "library_path": env["LD_LIBRARY_PATH"]})

    def execute(job):
        began = time.monotonic()
        stdout = output / "data/raw" / job["family"] / (job["name"] + ".out")
        stderr = stdout.with_suffix(".err")
        command = [str(binaries[job["target"]])] + [arg.replace("{OUTPUT}", str(output)) for arg in job["argv"]]
        if job["phase"] == "cpu-timing" and cpu is not None:
            command = [taskset, "-c", str(cpu)] + command
        record = {"job_id": job["id"], "family": job["family"], "target": job["target"],
                  "command": command, "binary_sha256": identities[job["target"]]["sha256"],
                  "started_utc": utc(), "stdout": stdout.relative_to(output).as_posix(),
                  "stderr": stderr.relative_to(output).as_posix()}
        with stdout.open("wb") as out, stderr.open("wb") as err:
            try:
                code = subprocess.run(command, cwd=work, env=env, stdout=out, stderr=err,
                                      timeout=args.timeout).returncode
            except subprocess.TimeoutExpired:
                code = 124
                record["failure"] = "Process exceeded --timeout and was killed"
            except OSError as exc:
                code = 127
                record["failure"] = str(exc)
        record.update(exit_code=code, finished_utc=utc(), elapsed_s=time.monotonic() - began,
                      stdout_sha256=sha(stdout), stderr_sha256=sha(stderr),
                      stdout_bytes=stdout.stat().st_size, stderr_bytes=stderr.stat().st_size)
        errors = check_output(job, stdout.read_text(encoding="utf-8", errors="replace"),
                              stderr.read_text(encoding="utf-8", errors="replace")) if code == 0 else []
        if job["family"] == "occupancy":
            generated = []
            for suffix in (".summary.csv", ".summary.json", ".residence.csv"):
                path = stdout.with_name(job["name"] + suffix)
                exists = path.is_file()
                if not exists:
                    errors.append("Missing occupancy output: " + path.name)
                generated.append({"path": path.relative_to(output).as_posix(), "exists": exists,
                                  "sha256": sha(path) if exists else None,
                                  "bytes": path.stat().st_size if exists else None})
            record["generated_files"] = generated
        record.update(passed=code == 0 and not errors, config_errors=errors)
        write_json(output / "processes" / job["family"] / (job["name"] + ".json"), record)
        return record

    records = []

    def completed(record):
        records.append(record)
        if len(records) % 25 == 0 or not record["passed"] or len(records) == len(jobs):
            print("{}/{} completed; {} failed; {}".format(len(records), len(jobs),
                  sum(not item["passed"] for item in records), record["job_id"]), flush=True)

    print("Running {} jobs ({}) with {} simulation workers".format(len(jobs), args.profile, args.jobs), flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(execute, job) for job in jobs if job["phase"] == "simulation"]
        for future in concurrent.futures.as_completed(futures):
            completed(future.result())
    if has_timing:
        print("Simulations finished; running CPU microbenchmarks serially", flush=True)
    for job in jobs:
        if job["phase"] == "cpu-timing":
            completed(execute(job))
    failures = sum(not record["passed"] for record in records)
    report = {"status": "failed" if failures else "passed", "complete": len(records) == len(jobs),
              "started_utc": started, "finished_utc": utc(), "process_count": len(records),
              "counts": counts(records), "expected_counts": counts(jobs), "failures": failures}
    write_json(output / "execution-summary.json", report)
    print("{}: {} ({} failures)".format(report["status"], output, failures), flush=True)
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("full", "quick"), default="full")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", "-j", type=int, default=8)
    parser.add_argument("--cpu", type=int, help="CPU for the serial microbenchmarks; default: first allowed CPU")
    parser.add_argument("--timeout", type=float, default=300, help="Seconds allowed per process (default: 300)")
    timing = parser.add_mutually_exclusive_group()
    timing.add_argument("--microbenchmark", dest="microbenchmark", action="store_true",
                        help="Include the optional host-software profile")
    timing.add_argument("--no-microbenchmark", dest="microbenchmark", action="store_false",
                        help="Run the selected simulations and functional checks")
    parser.set_defaults(microbenchmark=None)
    args = parser.parse_args()
    try:
        return run(args)
    except (ValueError, RuntimeError, OSError, KeyError) as exc:
        parser.exit(2, "error: {}\n".format(exc))


if __name__ == "__main__":
    raise SystemExit(main())
