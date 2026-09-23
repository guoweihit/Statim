# SPDX-License-Identifier: GPL-3.0-or-later
"""Statistical edge cases and grouping of configurable independent samples."""
import math
from pathlib import Path
import sys
import unittest

import numpy as np
from scipy import stats

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "analysis"))
from statistics_core import estimate, regression, summaries, paired_effects, delay_regressions, key


def record(seed, delay=0, enabled=True, topology="chain", value=1):
    return dict(target="statim-path", parameters=dict(run=seed, controller_delay_s=delay,
        temporary_forwarding=enabled, topology=topology), metrics={"delay_ms": value},
        experiments=["arbitrary-name"], analysis_views=["main", "replacement"])


class StatisticsTests(unittest.TestCase):
    def test_all_sample_sizes_use_actual_degrees_of_freedom(self):
        for n in (1, 2, 3, 7, 31):
            with self.subTest(n=n):
                values = np.arange(n, dtype=float)
                result = estimate(values)
                self.assertEqual(result["n"], n)
                self.assertEqual(result["mean"], float(np.mean(values)))
                if n == 1:
                    self.assertIsNone(result["ci_half_width"])
                    self.assertEqual(result["ci_status"], "insufficient_samples")
                else:
                    expected = stats.t.ppf(.975, n - 1) * stats.sem(values)
                    self.assertAlmostEqual(result["ci_half_width"], expected, places=12)

    def test_missing_invalid_and_zero_variance(self):
        result = estimate([1, 1, None, "", float("nan"), float("inf"), "bad"])
        self.assertEqual((result["n"], result["missing"], result["invalid"]), (2, 2, 3))
        self.assertEqual(result["ci_half_width"], 0)
        self.assertEqual(result["ci_status"], "zero_sample_variance")
        self.assertIsNone(estimate([])["mean"])

    def test_configurable_confidence(self):
        values = [2, 5, 8, 10, 17, 21, 28]
        result = estimate(values, confidence=.9)
        self.assertAlmostEqual(result["ci_half_width"], stats.t.ppf(.95, 6) * stats.sem(values))
        with self.assertRaises(ValueError):
            estimate(values, confidence=1)

    def test_equivalent_numeric_parameter_representations(self):
        self.assertEqual(key(dict(seed=1, delay=0)), key(dict(seed=1.0, delay=0.0)))
        self.assertNotEqual(key(dict(enabled=True)), key(dict(enabled=1)))

    def test_regression_degenerate_and_custom_points(self):
        with self.assertRaises(ValueError):
            regression([1, 2, 3], [3, 5, 8], confidence=1.2)
        self.assertEqual(regression([1], [3])["ci_status"], "insufficient_samples")
        self.assertEqual(regression([1, 1], [3, 4])["ci_status"], "constant_predictor")
        self.assertEqual(regression([1], [3])["r_squared_status"], "insufficient_samples")
        self.assertEqual(regression([1, 1], [3, 4])["r_squared_status"], "constant_predictor")
        two = regression([.2, 1.7], [3, 6])
        self.assertAlmostEqual(two["slope"], 2)
        self.assertIsNone(two["slope_ci_low"])
        result = regression([.01, .2, .7, float("nan")], [2.03, 2.6, 4.1, 20])
        self.assertEqual(result["n"], 3)
        self.assertEqual(result["missing_pairs"], 1)
        self.assertAlmostEqual(result["slope"], 3)
        constant = regression([1, 2, 3], [5, 5, 5])
        self.assertEqual(constant["slope_ci_low"], 0)
        self.assertEqual(constant["ci_status"], "zero_residual_variance")
        self.assertIsNone(constant["r_squared"])
        self.assertEqual(constant["r_squared_status"], "constant_response")

    def test_constant_response_has_explicit_undefined_r_squared(self):
        for value in (0, .1, 110.253):
            for xs in ([0, 1], [0, .05, .1, .25, .5, 1, 2]):
                with self.subTest(value=value, xs=xs):
                    result = regression(xs, [value] * len(xs))
                    self.assertEqual(result["slope"], 0)
                    self.assertIsNone(result["r_squared"])
                    self.assertEqual(result["r_squared_status"], "constant_response")
                    if len(xs) > 2:
                        self.assertEqual(result["slope_ci_low"], 0)
                        self.assertEqual(result["slope_ci_high"], 0)
                    else:
                        self.assertIsNone(result["slope_ci_low"])

    def test_nearly_perfect_regression_preserves_small_residual_uncertainty(self):
        # Symmetric residuals are orthogonal to the intercept and predictor.
        xs = np.array([-3, -2, -1, 0, 1, 2, 3], dtype=float)
        residuals = np.array([1, -1, 0, 0, 0, -1, 1], dtype=float) * 1e-5
        ys = 500 + 1000 * xs + residuals
        result = regression(xs, ys)
        expected_half = stats.t.ppf(.975, 5) * np.sqrt(
            np.dot(residuals, residuals) / 5 / np.dot(xs, xs))
        half = (result["slope_ci_high"] - result["slope_ci_low"]) / 2
        self.assertAlmostEqual(result["slope"], 1000)
        self.assertAlmostEqual(result["intercept"], 500)
        self.assertAlmostEqual(half / expected_half, 1, places=6)
        self.assertEqual(result["ci_status"], "estimated")
        self.assertEqual(result["r_squared_status"], "defined")

    def test_nearly_perfect_line_has_no_correlation_cancellation_interval(self):
        xs = [0, .05, .1, .25, .5, 1, 2]
        ys = [110.095 + 1000 * x for x in xs]
        result = regression(xs, ys)
        self.assertLess(result["slope_ci_high"] - result["slope_ci_low"], 1e-9)

    def test_residual_interval_matches_noisy_ols_and_custom_confidence(self):
        xs, ys = [0, 1, 2, 3, 4], [1, 2, 1, 4, 5]
        result = regression(xs, ys, confidence=.9)
        expected = stats.linregress(xs, ys)
        half = stats.t.ppf(.95, 3) * expected.stderr
        self.assertAlmostEqual(result["slope_ci_low"], expected.slope - half)
        self.assertAlmostEqual(result["slope_ci_high"], expected.slope + half)
        self.assertAlmostEqual(result["r_squared"], expected.rvalue ** 2)
        self.assertEqual(result["r_squared_status"], "defined")

    def test_parameters_keep_topology_and_feature_separate(self):
        rows = [record(seed, enabled=enabled, topology=topology, value=seed)
                for seed in (8, 11, 92) for enabled in (True, False) for topology in ("chain", "mesh")]
        result = summaries(rows)
        self.assertEqual(len(result), 4)
        self.assertTrue(all(item["n"] == 3 for item in result))
        self.assertTrue(all(item["analysis_views"] == ["main", "replacement"] for item in result))

    def test_seed_matched_effects_and_incomplete_pairs(self):
        rows = [record(seed, enabled=enabled, value=seed + 4 * enabled)
                for seed in (3, 10, 55) for enabled in (True, False)]
        rows.append(record(77, enabled=True, value=100))
        rows.append(dict(record(3), repetition_of="first"))
        result = paired_effects(rows)[0]
        self.assertEqual(result["n"], 3)
        self.assertEqual(result["mean"], 4)
        self.assertEqual(result["unmatched_seeds"], 1)

    def test_regression_uses_independent_seed_slopes(self):
        rows = [record(seed, delay=delay, value=5 + seed * delay)
                for seed in (2, 3, 7) for delay in (.03, .4, 1.3)]
        result = delay_regressions(rows)[0]
        self.assertEqual(result["slope"]["n"], 3)
        self.assertAlmostEqual(result["slope"]["mean"], 4)
        self.assertEqual(len(result["seed_fits"]), 3)
        self.assertEqual(result["seed_fits"][0]["predictor_values"], [.03, .4, 1.3])

    def test_ambiguously_repeated_seed_is_reported(self):
        rows = [record(1, enabled=True), record(1, enabled=True), record(1, enabled=False)]
        result = paired_effects(rows)[0]
        self.assertEqual(result["ambiguous_seeds"], 1)
        self.assertEqual(result["n"], 0)

    def test_disjoint_seeds_report_unmatched_coverage(self):
        rows = [record(10, enabled=True), record(20, enabled=False)]
        result = paired_effects(rows)[0]
        self.assertEqual(result["n"], 0)
        self.assertEqual(result["unmatched_seeds"], 2)
        self.assertEqual((result["enabled_seeds"], result["disabled_seeds"]), (1, 1))


if __name__ == "__main__":
    unittest.main()
