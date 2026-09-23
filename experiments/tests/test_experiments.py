# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import configparser
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import matrix
import run
from configuration import effective_experiments


class MatrixTests(unittest.TestCase):
    @staticmethod
    def parser():
        parser = configparser.ConfigParser(interpolation=None, strict=True)
        parser.optionxform = str
        parser.read(matrix.DEFAULT_CONFIG, encoding="utf-8-sig")
        return parser

    @staticmethod
    def load_ini(parser):
        stream = io.StringIO()
        parser.write(stream)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "实验配置.ini"
            path.write_text("# 中文说明：单位为秒。\n" + stream.getvalue(), encoding="utf-8-sig")
            return matrix.load_config(path)

    def test_supplied_counts_and_registries(self):
        jobs = matrix.expand(matrix.load_config())
        self.assertEqual(len(jobs), 3124)
        self.assertEqual(matrix.counts(jobs), dict(primary=2040, paths=720, occupancy=360,
                                                faults=1, lookup=1, boundary=2))
        self.assertEqual(len(matrix.STATE_CASES), 17)
        self.assertEqual(len(matrix.PACKET_GROUPS), 11)
        self.assertEqual(len(matrix.FAULT_CASES), 20)
        self.assertEqual(len({j["target"] for j in jobs}), 8)
        self.assertEqual(sum(len(j["experiments"]) > 1 for j in jobs), 120)

    def test_inheritance_selection_and_deduplication(self):
        config = matrix.load_config()
        self.assertEqual(config["experiments"]["handover_quality"], {})
        effective = {n: p for n, p, _ in effective_experiments(config)}
        self.assertEqual(effective["handover_quality"], config["defaults"])
        config["experiments"] = {"first": {}, "same_data": {"analysis_views": ["replacement"]},
                                 "later": {"enabled": False}}
        config["checks"]["enabled"] = False
        jobs = matrix.expand(config, "quick")
        self.assertEqual(len(jobs), 30)
        self.assertTrue(all(j["experiments"] == ["first", "same_data"] for j in jobs))
        self.assertTrue(all("replacement" in j["analysis_views"] for j in jobs))
        self.assertEqual(len({(j["target"], tuple(j["argv"])) for j in jobs}), len(jobs))

    def test_unicode_multiline_and_scalar_dimensions(self):
        parser = self.parser()
        parser["defaults"]["controller_delay_s"] = "[\n  0,\n  0.25,\n  1.5\n]"
        parser["defaults"]["generation_guard"] = "true"
        parser["experiment.path_sensitivity"]["consumer_retransmissions"] = "false"
        config = self.load_ini(parser)
        self.assertEqual(config["defaults"]["controller_delay_s"], [0, 0.25, 1.5])
        self.assertIs(config["defaults"]["generation_guard"], True)
        self.assertTrue(matrix.expand(config, "quick"))

    def test_section_spelling_and_required_parameters(self):
        mutations = [
            lambda p: p.add_section("unknown"),
            lambda p: p.remove_section("defaults"),
            lambda p: p["defaults"].__setitem__("controller_delai_s", "[0]"),
            lambda p: p["defaults"].__delitem__("controller_delay_s"),
            lambda p: p["DEFAULT"].__setitem__("hidden", "1"),
            lambda p: p["defaults"].__setitem__("Seed_first", "1"),
            lambda p: p.add_section("experiment."),
        ]
        for change in mutations:
            parser = self.parser()
            change(parser)
            with self.assertRaises(ValueError):
                self.load_ini(parser)

    def test_duplicate_ini_keys(self):
        text = matrix.DEFAULT_CONFIG.read_text(encoding="utf-8-sig")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate.ini"
            path.write_text(text.replace("seed_first = 1", "seed_first = 1\nseed_first = 2", 1), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "already exists"):
                matrix.load_config(path)

    def test_invalid_finite_and_boolean_values(self):
        for raw in ("NaN", "Infinity", "-Infinity", "1e999", '"1"', "null", "true"):
            parser = self.parser()
            parser["defaults"]["controller_delay_s"] = "[" + raw + "]"
            with self.assertRaises(ValueError):
                self.load_ini(parser)
        for section, key in (("defaults", "generation_guard"), ("defaults", "consumer_retransmissions"),
                             ("defaults", "interest_reforwarding"), ("experiment.handover_quality", "enabled")):
            for raw in ("0", "1", '"false"', "False", "yes"):
                parser = self.parser()
                parser[section][key] = raw
                with self.assertRaises(ValueError):
                    self.load_ini(parser)

    def test_parameter_ranges_types_and_relationships(self):
        invalid = [
            ("defaults", "seed_first", "0"), ("defaults", "seed_last", "4294967296"),
            ("defaults", "controller_delay_s", "[-0.1]"), ("defaults", "controller_delay_s", "[]"),
            ("defaults", "systems", '["statmi"]'), ("defaults", "systems", "[{}]"),
            ("defaults", "consumer_join", '["center"]'), ("defaults", "trace_path_hops", "[1]"),
            ("defaults", "trace_path_hops", "[2.5]"), ("defaults", "payload_bytes", "1.5"),
            ("defaults", "consumer_cbr_frequency", "0"), ("defaults", "interest_lifetime_s", "0"),
            ("defaults", "initial_rtt_estimate_s", "0"), ("defaults", "controller_retry_limit", "-1"),
            ("defaults", "completion_notice_loss", "[1.01]"), ("defaults", "retry_timeout_margin_s", "-0.1"),
            ("defaults", "arrival_rate_per_s", "[0]"), ("defaults", "arrival_count", "2.5"),
            ("defaults", "initial_burst", "[true]"), ("defaults", "handover_base_s", "0"),
            ("defaults", "stop_s", "27"),
            ("experiment.occupancy", "controller_delay_s", "[0]"),
            ("experiment.occupancy_burst", "initial_burst", "[10001]"),
            ("experiment.path_sensitivity", "systems", '["nfd-kite"]'),
            ("experiment.path_sensitivity", "trace_lifetime_s", "18"),
            ("experiment.path_sensitivity", "refresh_interval_s", "18"),
            ("microbenchmark", "batch", "10001"), ("microbenchmark", "invocations", "0"),
            ("quick", "occupancy_arrival_count", "0"), ("checks", "programs", '["missing"]'),
            ("experiment.handover_quality", "analysis_views", '"one"'),
        ]
        for section, key, value in invalid:
            with self.subTest(section=section, key=key, value=value):
                parser = self.parser()
                parser[section][key] = value
                with self.assertRaises(ValueError):
                    self.load_ini(parser)

    def test_custom_seeds_and_optional_timing(self):
        config = matrix.load_config()
        config["defaults"].update(seed_first=7, seed_last=8)
        jobs = matrix.expand(config)
        self.assertEqual(len(jobs), 212)
        self.assertEqual({j["parameters"]["seed"] for j in jobs if "seed" in j["parameters"]}, {7, 8})
        quick = matrix.expand(config, "quick")
        self.assertEqual(len(quick), 108)
        self.assertEqual({j["parameters"]["seed"] for j in quick if "seed" in j["parameters"]}, {7})
        timed = matrix.expand(config, "quick", microbenchmark=True)
        self.assertEqual(len(timed), 110)
        timing = [j for j in timed if j["phase"] == "cpu-timing"]
        self.assertEqual(len({j["id"] for j in timing}), 2)
        self.assertTrue(all(j["argv"] == ["512", "2048", "1", "64"] for j in timing))
        config["microbenchmark"]["enabled"] = True
        self.assertEqual(len(matrix.expand(config, "quick")), 110)
        self.assertEqual(len(matrix.expand(config, "quick", microbenchmark=False)), 108)

    def test_common_expiry_main_and_boundary_scopes(self):
        config = matrix.load_config()
        self.assertEqual(config['defaults']['trace_lifetime_s'], 5)
        self.assertEqual(config['defaults']['refresh_interval_s'], 2)
        self.assertEqual(config['defaults']['interest_lifetime_s'], 2)
        jobs = matrix.expand(config)
        boundary = [j for j in jobs if 'route_expiry_boundary' in j['experiments']]
        self.assertEqual(len(boundary), 180)
        self.assertEqual({j['parameters']['controller_delay_s'] for j in boundary}, {0, 0.5, 2})
        self.assertEqual({j['parameters']['temporary_forwarding'] for j in boundary}, {False, True})
        self.assertTrue(all(j['parameters']['trace_lifetime_s'] == j['parameters']['refresh_interval_s'] == 2 for j in boundary))
        self.assertTrue(all(not j['parameters']['consumer_retransmissions'] and not j['parameters']['interest_reforwarding'] for j in boundary))
        self.assertTrue(all(len(j['experiments']) == 1 for j in boundary))

    def test_effective_arguments_and_independent_durations(self):
        config = matrix.load_config()
        config["experiments"]["completion_notice_loss"].update(controller_delay_s=[0.75], completion_notice_loss=[0.2])
        config["experiments"]["path_sensitivity"]["trace_path_hops"] = [6]
        config["experiments"]["occupancy"]["arrival_rate_per_s"] = [25]
        config["defaults"].update(interest_lifetime_s=3.5, initial_rtt_estimate_s=0.123456789)
        jobs = matrix.expand(config, "quick")
        fault = next(j for j in jobs if "completion_notice_loss" in j["experiments"])
        self.assertIn("--controllerDelay=0.75", fault["argv"])
        self.assertIn("--controlLossRate=0.2", fault["argv"])
        self.assertIn("--controllerRetryTimeout=1.55", fault["argv"])
        path = next(j for j in jobs if j["family"] == "paths" and j["parameters"]["consumer_join"] == "middle")
        self.assertEqual(path["expected_config"]["joinIndex"], "3")
        self.assertTrue(any("--lambda=25" in j["argv"] for j in jobs if j["family"] == "occupancy"))
        for job in jobs:
            if job["family"] in ("primary", "paths"):
                self.assertIn("--interestLifetime=3.5", job["argv"])
                self.assertIn("--initialRttEstimate=0.123456789", job["argv"])
                self.assertEqual(job["parameters"]["initial_rtt_estimate_s"], 0.123456789)

    def test_quick_burst_preservation(self):
        config = matrix.load_config()
        bursts = {j["parameters"]["initial_burst"]: j["parameters"]["arrival_count"]
                  for j in matrix.expand(config, "quick") if j["family"] == "occupancy"}
        self.assertEqual(bursts, {0: 100, 32: 100, 128: 129, 512: 513})
        config["defaults"]["arrival_count"] = 512
        for job in matrix.expand(config, "quick"):
            if job["family"] == "occupancy":
                self.assertLessEqual(job["parameters"]["initial_burst"], job["parameters"]["arrival_count"])
                self.assertLessEqual(job["parameters"]["arrival_count"], 512)
        config["defaults"]["arrival_count"] = 100
        with self.assertRaises(ValueError):
            matrix.expand(config, "quick")

    def test_switches_and_nfd_effective_parameters(self):
        config = matrix.load_config()
        config["experiments"] = {"all_switches": {"interest_reforwarding": [True, False]}}
        jobs = matrix.expand(config, "quick")
        self.assertEqual(sum(j["target"] == "kite-grid" for j in jobs), 4)
        for job in jobs:
            if job["family"] == "primary":
                p = job["parameters"]
                self.assertIn("--consumerRetransmissions=" + str(int(p["consumer_retransmissions"])), job["argv"])
                self.assertIn("--enableInterestReforwarding=" + str(int(p["interest_reforwarding"])), job["argv"])
                if job["target"] == "kite-grid":
                    self.assertNotIn("controller_delay_s", p)
                    self.assertNotIn("temporary_forwarding", p)

    def test_unsupported_explicit_overrides_and_wire_time_resolution(self):
        invalid = [
            ("experiment.path_sensitivity", "generation_guard", "true"),
            ("experiment.path_sensitivity", "interest_reforwarding_limit", "7"),
            ("experiment.path_sensitivity", "completion_notice_loss", "[0.5]"),
            ("experiment.occupancy", "systems", '["nfd-kite"]'),
            ("experiment.occupancy", "payload_bytes", "800"),
            ("experiment.handover_quality", "trace_path_hops", "[4]"),
            ("defaults", "interest_lifetime_s", "0.0001"),
            ("defaults", "interest_lifetime_s", "0.1234"),
            ("defaults", "trace_lifetime_s", "0.1234"),
            ("defaults", "payload_bytes", "2147483648"),
        ]
        for section, key, raw in invalid:
            with self.subTest(section=section, key=key, raw=raw):
                parser = self.parser()
                parser[section][key] = raw
                with self.assertRaises(ValueError):
                    self.load_ini(parser)
        config = matrix.load_config()
        config["experiments"] = {"baseline": {"systems": ["nfd-kite"], "stateful_delay_us": [1]}}
        with self.assertRaises(ValueError):
            matrix.expand(config)
        config["experiments"] = {"baseline": {"systems": ["nfd-kite"]}}
        config["defaults"].update(interest_lifetime_s=0.001, initial_rtt_estimate_s=0.123456789)
        self.assertTrue(matrix.expand(config, "quick"))

    def test_output_checks(self):
        jobs = matrix.expand(matrix.load_config(), "quick")
        state = next(j for j in jobs if j["target"] == "state-table-tests")
        self.assertEqual(run.check_output(state, "\n".join("PASS " + c for c in matrix.STATE_CASES), ""), [])
        self.assertTrue(run.check_output(state, "PASS " + matrix.STATE_CASES[0], ""))
        job = jobs[0]
        config_line = "[CONFIG] " + " ".join(k + "=" + v for k, v in job["expected_config"].items())
        self.assertEqual(run.check_output(job, "", config_line), [])
        self.assertTrue(run.check_output(job, "", config_line.replace("run=1", "run=9")))

    def test_runner_direct_execution_and_optional_timing(self):
        for timing in (False, True):
            with self.subTest(timing=timing), tempfile.TemporaryDirectory() as temporary:
                root, output = Path(temporary) / "ns3", Path(temporary) / "results"
                topology = root / run.TOPOLOGY
                topology.parent.mkdir(parents=True)
                topology.write_text("test topology", encoding="utf-8")
                jobs = [dict(id="primary/sim", family="primary", name="sim", target="statim-grid",
                             argv=["--run=7"], expected_config={}, phase="simulation", experiments=["example"])]
                if timing:
                    jobs.append(dict(id="microbenchmark/cpu", family="microbenchmark", name="cpu",
                                     target="software-microbenchmark", argv=["512", "2048", "1", "64"],
                                     expected_config={}, phase="cpu-timing", experiments=["software_profile"]))
                commands = []

                def fake_process(command, **kwargs):
                    commands.append(command)
                    self.assertEqual(kwargs["cwd"], output / "work")
                    kwargs["stdout"].write(b"mock process\n")
                    return argparse.Namespace(returncode=7 if "--run=7" in command else 0)

                binaries = {j["target"]: Path(sys.executable) for j in jobs}
                args = argparse.Namespace(config=matrix.DEFAULT_CONFIG, profile="quick", output=output,
                                          jobs=2, cpu=0, timeout=5, microbenchmark=timing)
                with contextlib.ExitStack() as stack:
                    for context in (
                        patch.object(run, "ROOT", root), patch.object(run, "expand", return_value=jobs),
                        patch.object(run, "find_binaries", return_value=binaries),
                        patch.object(run.os, "sched_getaffinity", return_value={0}, create=True),
                        patch.object(run.shutil, "which", return_value="taskset" if timing else None),
                        patch.object(run.subprocess, "run", side_effect=fake_process),
                        contextlib.redirect_stdout(io.StringIO()),
                    ):
                        stack.enter_context(context)
                    self.assertEqual(run.run(args), 1)
                    with self.assertRaises(ValueError):
                        run.run(args)
                self.assertEqual(commands[0], [sys.executable, "--run=7"])
                if timing:
                    self.assertEqual(commands[1][:3], ["taskset", "-c", "0"])
                summary = json.loads((output / "execution-summary.json").read_text())
                self.assertEqual(summary["failures"], 1)
                self.assertTrue(summary["complete"])
                manifest = json.loads((output / "run-manifest.json").read_text())
                self.assertEqual(manifest["experiment_counts"]["example"], 1)


if __name__ == "__main__":
    unittest.main()
