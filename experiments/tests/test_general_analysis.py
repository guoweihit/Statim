# SPDX-License-Identifier: GPL-3.0-or-later
"""Manifest and raw-data tests for configurable experiment analysis."""
import csv
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "analysis"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dataset import load_run, analyze, timing_invocation_means, save_analysis, compare_datasets
from observations import simulation, occupancy, microbenchmark, parameters
from matrix import expand, load_config
from statistics_core import key


def text_log(handover=3.5, value=110):
    return ("[CONFIG] scenario=statim-grid t_ho={}\n"
        "[HO_DELAY] TI_sent=0.2 delay=800ms\n"
        "[HO_DELAY] TI_sent={} delay={}ms\n"
        "[WINDOW_LOSS] sent_in_window=31 never_satisfied=2 frac_pct=6.4516\n"
        "[RUN_LOSS] unique_sent=100 never_satisfied_total=4\n").format(handover, handover, value)


def add_process(root, job, text):
    stdout, stderr = Path("data/raw") / job["family"] / (job["name"] + ".out"), Path("data/raw") / job["family"] / (job["name"] + ".err")
    (root / stdout).parent.mkdir(parents=True, exist_ok=True)
    (root / stdout).write_bytes(b"")
    (root / stderr).write_text(text, encoding="utf-8")
    process = dict(job_id=job["id"], exit_code=0, passed=True,
                   stdout=stdout.as_posix(), stderr=stderr.as_posix(), binary_sha256="fixed")
    for field, path in (("stdout", stdout), ("stderr", stderr)):
        payload = (root / path).read_bytes()
        process[field + "_bytes"] = len(payload)
        process[field + "_sha256"] = hashlib.sha256(payload).hexdigest()
    path = root / "processes" / job["family"] / (job["name"] + ".json")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(process), encoding="utf-8")


class GeneralAnalysisTests(unittest.TestCase):
    def test_matrix_manifest_custom_delays_seeds_and_independent_lifetimes(self):
        config = load_config()
        config["defaults"].update(systems=["statim"], seed_first=12, seed_last=14,
            consumer_retransmissions=[False], controller_delay_s=[.03, .17, .89],
            temporary_forwarding=[True, False], interest_lifetime_s=3,
            initial_rtt_estimate_s=.25)
        config["experiments"] = {"base": {}, "rtt": {"initial_rtt_estimate_s": .8},
                                 "lifetime": {"interest_lifetime_s": 5}}
        config["checks"]["enabled"] = False
        jobs = expand(config)
        self.assertEqual(len(jobs), 54)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for job in jobs:
                settings = job["parameters"]
                value = (100 + settings["seed"] * settings["controller_delay_s"]
                    + 10 * settings["temporary_forwarding"] + settings["interest_lifetime_s"]
                    + 100 * settings["initial_rtt_estimate_s"])
                add_process(root, job, text_log(value=value))
            (root / "run-manifest.json").write_text(json.dumps(dict(profile="full", jobs=jobs)), encoding="utf-8")
            (root / "run-start.json").write_text(json.dumps(dict(binaries={"statim-grid": {"sha256": "fixed"}})), encoding="utf-8")
            data = load_run(root, verify=True)
            self.assertEqual(data["issues"], [])
            results = analyze(data)
            select = lambda rows: [row for row in rows if row["metric"] == "handover_delay_ms"]
            summary, paired, regressions = map(select,
                (results["summary"], results["paired"], results["regression"]))
            self.assertEqual(len(summary), 18)
            self.assertEqual({row["n"] for row in summary}, {3})
            self.assertEqual({(row["parameters"]["interest_lifetime_s"],
                              row["parameters"]["initial_rtt_estimate_s"]) for row in summary},
                             {(3, .25), (3, .8), (5, .25)})
            self.assertEqual(len(paired), 9)
            self.assertTrue(all(row["n"] == 3 and abs(row["mean"] - 10) < 1e-10 for row in paired))
            self.assertEqual(len(regressions), 6)
            self.assertTrue(all(row["slope"]["n"] == 3 for row in regressions))
            self.assertTrue(all(abs(row["slope"]["mean"] - 13) < 1e-10 for row in regressions))

    def test_current_manifest_and_argv_have_equivalent_effective_parameters(self):
        for job in expand(load_config(), "quick", microbenchmark=True):
            with self.subTest(target=job["target"], job=job["id"]):
                argv_job = {name: value for name, value in job.items() if name != "parameters"}
                expected = {name: value for name, value in job["parameters"].items() if name != "invocation"}
                self.assertEqual(key(parameters(argv_job)), key(expected))

    def test_historical_shared_lifetime_and_parameter_vocabulary(self):
        old = dict(target="statim-grid", argv=["--run=19", "--retxTime=2", "--controlLossRate=0.1"])
        settings = parameters(old)
        self.assertEqual(settings["interest_lifetime_s"], 2)
        self.assertEqual(settings["initial_rtt_estimate_s"], 2)
        self.assertEqual(settings["seed"], 19)
        current = dict(records=[dict(target="statim-grid", parameters=settings, metrics={"value": 3})])
        historical = dict(records=[dict(target="statim-grid", parameters=dict(run=19,
            interest_lifetime_s=2, initial_rtt_estimate_s=2,
            completion_notice_loss_probability=.1), metrics={"value": 3})])
        result = compare_datasets(current, historical)
        self.assertEqual(result["matched_runs"], 1)
        self.assertEqual(result["differences"], [])
        with self.assertRaisesRegex(ValueError, "disagree"):
            parameters(dict(target="statim-grid", parameters=dict(seed=3, run=4)))

    def test_absent_invalid_and_failed_observations_have_separate_counts(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            jobs = [dict(id="custom/" + str(seed), family="custom", name=str(seed),
                         target="statim-grid", parameters=dict(seed=seed)) for seed in range(4)]
            for seed, job in enumerate(jobs):
                log = text_log()
                if seed == 1:
                    log = log.replace("frac_pct=6.4516", "frac_pct=nan")
                elif seed == 2:
                    log = log.replace("frac_pct=6.4516", "")
                add_process(root, job, log)
                if seed == 3:
                    path = root / "processes/custom/3.json"
                    payload = json.loads(path.read_text(encoding="utf-8"))
                    payload.update(passed=False, exit_code=1)
                    path.write_text(json.dumps(payload), encoding="utf-8")
            (root / "run-manifest.json").write_text(json.dumps(dict(jobs=jobs)), encoding="utf-8")
            data = load_run(root)
            summary = next(row for row in analyze(data)["summary"] if row["metric"] == "window_unsatisfied_pct")
            self.assertEqual((summary["total"], summary["n"], summary["missing"], summary["invalid"]), (3, 1, 1, 1))
            self.assertEqual(summary["ci_status"], "insufficient_samples")
            self.assertEqual(sum(row["status"] == "failed" for row in data["process_reports"]), 1)
            self.assertEqual(len([issue for issue in data["issues"] if issue["severity"] == "error"]), 3)
            self.assertEqual(data["records"][1]["metric_states"]["window_unsatisfied_pct"], "invalid")
            json.dumps(data, allow_nan=False)

    def test_custom_manifest_dimensions_and_no_microbenchmark(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            jobs = []
            for seed in (12, 25, 99):
                for delay in (.03, .17, .89):
                    name = "opaque-{}-{}".format(seed, delay)
                    job = dict(id="custom/" + name, family="custom", name=name,
                        target="statim-grid", parameters=dict(run=seed, system="statim",
                            temporary_forwarding=True, topology="user-topology",
                            controller_delay_s=delay), experiments=["new-experiment"],
                            analysis_views=["handover", "replacement"])
                    jobs.append(job)
                    add_process(root, job, text_log(value=100 + seed * delay))
            (root / "run-manifest.json").write_text(json.dumps(dict(profile="custom", jobs=jobs)), encoding="utf-8")
            (root / "run-start.json").write_text(json.dumps(dict(binaries={"statim-grid": {"sha256": "fixed"}})), encoding="utf-8")
            dataset = load_run(root, verify=True)
            self.assertEqual(dataset["issues"], [])
            self.assertEqual(len(dataset["records"]), 9)
            results = analyze(dataset)
            handover = [row for row in results["summary"] if row["metric"] == "handover_delay_ms"]
            self.assertEqual(len(handover), 3)
            self.assertTrue(all(row["n"] == 3 for row in handover))
            regression = next(row for row in results["regression"] if row["metric"] == "handover_delay_ms")
            self.assertAlmostEqual(regression["slope"]["mean"], (12 + 25 + 99) / 3)
            self.assertEqual(regression["slope"]["n"], 3)
            report = save_analysis(dataset, root / "tables", root / "report.json")
            self.assertEqual(report["status"], "completed")
            self.assertTrue((root / "tables/summary.csv").exists())
            # Verification concerns the exact recorded output bytes.
            path = root / "data/raw/custom" / (jobs[0]["name"] + ".err")
            path.write_text(text_log(value=999), encoding="utf-8")
            self.assertTrue(any(item["severity"] == "error" for item in load_run(root, True)["issues"]))

    def test_handover_timestamp_comes_from_recorded_configuration(self):
        for instant in (1.7, 6.2, 23.7):
            metrics, _, issues = simulation(text_log(handover=instant))
            self.assertEqual(metrics["handover_delay_ms"], 110)
            self.assertEqual(issues, [])

    def test_censored_handover_remains_explicitly_missing(self):
        text = text_log().replace("[HO_DELAY] TI_sent=3.5 delay=110ms\n", "")
        metrics, _, issues = simulation(text)
        self.assertIsNone(metrics["handover_delay_ms"])
        self.assertEqual(issues[0]["reason"], "handover_completion_unobserved")

    def test_occupancy_accepts_actual_arrival_count(self):
        for count in (3, 7, 31):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as temp:
                row = dict(seed=9, lambda_per_s=2, tc_s=.3, burst_count=0, arrival_count=count,
                    last_arrival_s=(count - 1) * .2, horizon_s=(count - 1) * .2 + .3,
                    high_water=2, mean_residence_s=.3, occupancy_area_s=count * .3)
                row["time_weighted_mean"] = row["occupancy_area_s"] / row["horizon_s"]
                parameters = {name: row[name] for name in ("seed", "lambda_per_s", "tc_s", "burst_count", "arrival_count")}
                metrics = {name: value for name, value in row.items() if name not in parameters}
                text = ",".join(row) + "\n" + ",".join(str(value) for value in row.values()) + "\n"
                payload = json.dumps(dict(parameters=parameters, metrics=metrics, implementation_checks={}))
                path = Path(temp) / "residence.csv"
                with path.open("w", newline="", encoding="utf-8") as stream:
                    writer = csv.writer(stream)
                    writer.writerow(["seed", "prefix_id", "arrival_s", "completion_s", "residence_s"])
                    writer.writerows([9, index, index * .2, index * .2 + .3, .3] for index in range(count))
                _, observations, issues = occupancy(text, payload, path)
                self.assertEqual(issues, [])
                self.assertEqual(observations[-1]["samples"], count)

    def test_nondefault_timing_repetitions_use_invocation_means(self):
        records = []
        for invocation in range(3):
            for repetition in range(7):
                records.append(dict(target="software-microbenchmark", job_id=str(invocation),
                    parameters=dict(invocation=invocation, repetition=repetition, benchmark="lookup", table_size=17),
                    metrics={"mean_ns": invocation + repetition}, experiments=[], analysis_views=[]))
        reduced = timing_invocation_means(records)
        self.assertEqual(len(reduced), 3)
        self.assertEqual([row["metrics"]["mean_ns"] for row in reduced], [3, 4, 5])
        summary = analyze(dict(records=records))["summary"][0]
        self.assertEqual(summary["n"], 3)
        self.assertEqual(summary["mean"], 4)


if __name__ == "__main__":
    unittest.main()
