# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression checks for complete historical log comparisons and analysis status."""
import contextlib
import io
import json
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True
EXPERIMENTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(EXPERIMENTS))
sys.path.insert(0, str(EXPERIMENTS / "analysis"))
import analyze
import compare_logs


def archive_logs(path, members):
    with tarfile.open(path, "w:gz") as archive:
        for name, payload in members:
            member = tarfile.TarInfo(name)
            member.size = len(payload)
            archive.addfile(member, io.BytesIO(payload))


class ReferenceLogTests(unittest.TestCase):
    def test_directory_and_archive_have_equivalent_comparisons(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fresh, reference = root / "fresh", root / "reference"
            fresh.mkdir()
            reference.mkdir()
            logs = [("run-a.err", b"[STATIM_COUNTERS] controlPkts=7\n"),
                    ("run-b.err", b"[HO_DELAY] delay=12ms\n")]
            for name, payload in logs:
                (fresh / name).write_bytes(payload)
                (reference / name).write_bytes(payload)
            # Select stderr evidence from expanded run directories.
            (reference / "run-a.out").write_bytes(b"")
            archive = root / "logs.tar.gz"
            archive_logs(archive, logs)
            directory_result = compare_logs.compare_family(fresh, reference)
            archive_result = compare_logs.compare_family(fresh, archive)
            for result in (directory_result, archive_result):
                self.assertTrue(result["text_equal"])
                self.assertEqual(result["byte_identical_logs"], 2)
                result.pop("historical_source")
                result.pop("historical_layout")
            self.assertEqual(directory_result, archive_result)

    def test_reference_selection_and_missing_or_empty_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(FileNotFoundError):
                compare_logs.reference_logs(root, "primary")
            with self.assertRaises(FileNotFoundError):
                compare_logs.compare_family(root, root / "missing.tar.gz")
            directory = root / "raw/primary"
            directory.mkdir(parents=True)
            with self.assertRaises(ValueError):
                compare_logs.compare_family(root, directory)
            archive = root / "raw/primary-logs.tar.gz"
            archive_logs(archive, [("run.err", b"value=1\n")])
            self.assertEqual(compare_logs.reference_logs(root, "primary"), directory)
            directory.rmdir()
            self.assertEqual(compare_logs.reference_logs(root, "primary"), archive)

    def test_archive_requires_unique_flat_regular_file_members(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "logs.tar.gz"
            for members in ([('nested/run.err', b'one\n')],
                            [('run.err', b'one\n'), ('run.err', b'two\n')]):
                archive_logs(archive, members)
                with self.assertRaises(ValueError):
                    compare_logs.compare_family(root, archive)

    def test_complete_comparison_reports_unknown_fields_and_missing_logs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fresh, reference = root / "fresh", root / "reference"
            fresh.mkdir()
            reference.mkdir()
            (reference / "field.err").write_bytes(b"[CUSTOM_OBSERVATION] experimentValue=7\n")
            (fresh / "field.err").write_bytes(b"[CUSTOM_OBSERVATION] measuredValue=7\n")
            (reference / "changed.err").write_bytes(b"metric=1\n")
            (fresh / "changed.err").write_bytes(b"metric=2\n")
            (reference / "missing.err").write_bytes(b"metric=3\n")
            (fresh / "unexpected.err").write_bytes(b"metric=4\n")
            result = compare_logs.compare_family(fresh, reference)
            self.assertFalse(result["text_equal"])
            self.assertEqual(result["missing_current_logs"], ["missing.err"])
            self.assertEqual(result["unexpected_current_logs"], ["unexpected.err"])
            self.assertEqual([item["name"] for item in result["different_logs"]],
                             ["changed.err", "field.err"])

    def test_complete_line_comparison_preserves_values_tags_and_whitespace(self):
        reference = b"[OBSERVATION] measuredValue=7 note=ready\n"
        variants = (b"[OBSERVATION] measuredValue=8 note=ready\n",
                    b"[ANOTHER_OBSERVATION] measuredValue=7 note=ready\n",
                    b"[OBSERVATION] measuredValue=7 note=ready extra=1\n",
                    b"[OBSERVATION] measuredValue=7  note=ready\n",
                    b"[OBSERVATION] measuredValue=7 note=ready\nadditional line\n")
        for current in variants:
            with self.subTest(current=current):
                result = compare_logs.compare_payloads(current, reference)
                self.assertFalse(result["text_equal"])
                self.assertFalse(result["raw_byte_identical"])
                self.assertTrue(result["differences"])
        result = compare_logs.compare_payloads(reference, reference)
        self.assertTrue(result["text_equal"])
        self.assertTrue(result["raw_byte_identical"])
        self.assertEqual(result["differences"], [])

    def test_line_endings_have_separate_byte_and_text_results(self):
        result = compare_logs.compare_payloads(b"value=7\r\n", b"value=7\n")
        self.assertTrue(result["text_equal"])
        self.assertFalse(result["raw_byte_identical"])
        self.assertEqual(result["historical_line_count"], 1)
        self.assertEqual(result["current_line_count"], 1)

    def test_execution_status_and_scientific_observations_are_separate(self):
        from dataset import save_analysis
        for severity in (None, "warning", "error"):
            with self.subTest(severity=severity), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                output = root / "analysis.json"
                dataset = dict(root=str(root), manifest_jobs=1, records=[],
                    process_reports=[dict(job_id="case", status="loaded")],
                    issues=[] if severity is None else [dict(severity=severity, reason="test")],
                    observations=[dict(name="constructed_counterexample", holds=False)],
                    repeatability=[], raw_verified=False)
                with patch.object(analyze, "load_run", return_value=dataset), \
                        contextlib.redirect_stdout(io.StringIO()):
                    returncode = analyze.main(["--run-root", str(root), "--output", str(output)])
                report = json.loads(output.read_text())
                self.assertEqual(returncode, 1 if severity == "error" else 0)
                self.assertEqual(report["status"], "data_errors" if severity == "error" else "completed")
                self.assertEqual(report["mechanism_observations"]["negative"], 1)
                self.assertEqual(report["errors"], int(severity == "error"))

    def test_historical_parameter_differences_are_reported(self):
        from dataset import compare_datasets
        current = dict(records=[dict(target="statim-grid", parameters=dict(seed=3, delay=.7),
                                      metrics=dict(handover_delay_ms=110))])
        reference = dict(records=[dict(target="statim-grid", parameters=dict(seed=3, delay=.7),
                                        metrics=dict(handover_delay_ms=112)),
                                  dict(target="statim-grid", parameters=dict(seed=9, delay=.2),
                                       metrics=dict(handover_delay_ms=105))])
        report = compare_datasets(current, reference)
        self.assertEqual(report["matched_runs"], 1)
        self.assertEqual(report["historical_only"], 1)
        self.assertEqual(len(report["differences"]), 1)
        self.assertEqual(report["differences"][0]["metric"], "handover_delay_ms")

if __name__ == "__main__":
    unittest.main()
