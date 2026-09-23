# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep historical FIB matches separate from current per-egress sends."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "analysis"))
from observations import fib_counter_metrics, simulation


BASE_LOG = """[CONFIG] scenario=statim-grid t_ho=10.123456789
[HO_DELAY] TI_sent=10.133456789 delay=110ms
[WINDOW_LOSS] window_start=10.123456789s window_len=4.000000000s sent_in_window=4000 never_satisfied=70 frac_pct=1.7500
[RUN_LOSS] unique_sent=30000 never_satisfied_total=70
"""
COMMON_COUNTERS = ("controlPkts=1 flowMods=1 syncCompletions=1 pullsSent=3 "
                   "controllerRetries=0 temporaryRouteWithdrawals=0 acksDropped=0 ")


class FibCounterObservationsTests(unittest.TestCase):
    def test_current_per_egress_counts_are_summed_over_nodes(self):
        lines = ["temporaryFibInterestTransmissions=4 flowTableFibInterestTransmissions=7",
                 "temporaryFibInterestTransmissions=2 flowTableFibInterestTransmissions=5"]
        log = BASE_LOG + "".join("[STATIM_COUNTERS] " + COMMON_COUNTERS + line + "\n"
                                 for line in lines)
        metrics, _, issues = simulation(log)
        self.assertEqual(issues, [])
        self.assertEqual(metrics["temporary_fib_interest_transmissions"], 6)
        self.assertEqual(metrics["flow_table_fib_interest_transmissions"], 12)
        self.assertEqual(metrics["reforwarded_interests"], 6)
        self.assertNotIn("legacy_temporary_fib_matches", metrics)
        self.assertNotIn("temporary_fib_forwards", metrics)

    def test_historical_counts_retain_their_overlapping_populations(self):
        log = BASE_LOG + ("[STATIM_COUNTERS] " + COMMON_COUNTERS +
                          "temporaryFibForwards=4 flowTableFibForwards=11\n")
        metrics, _, issues = simulation(log)
        self.assertEqual(issues, [])
        self.assertEqual(metrics["legacy_temporary_fib_matches"], 4)
        self.assertEqual(metrics["legacy_interest_transmissions"], 11)
        self.assertNotIn("temporary_fib_interest_transmissions", metrics)
        self.assertNotIn("flow_table_fib_interest_transmissions", metrics)
        self.assertNotIn("flow_table_fib_forwards", metrics)

    def test_missing_member_of_current_schema_is_reported(self):
        issues = []
        metrics = fib_counter_metrics([{"temporaryFibInterestTransmissions": "4"}], issues)
        self.assertEqual(metrics["temporary_fib_interest_transmissions"], 4)
        self.assertIsNone(metrics["flow_table_fib_interest_transmissions"])
        self.assertEqual(issues[0]["field"], "flow_table_fib_interest_transmissions")

    def test_mixed_node_schemas_do_not_silently_add_different_units(self):
        issues = []
        metrics = fib_counter_metrics([
            {"temporaryFibForwards": "4", "flowTableFibForwards": "11"},
            {"temporaryFibInterestTransmissions": "2", "flowTableFibInterestTransmissions": "5"},
        ], issues)
        self.assertTrue(all(value is None for value in metrics.values()))
        self.assertEqual(len(issues), 4)

    def test_path_totals_use_the_same_explicit_counter_schemas(self):
        base = BASE_LOG.replace("scenario=statim-grid", "scenario=statim-path tracePathHops=0")
        common = ("[PATH_TOTAL] controls=1 flowMods=1 packetIns=1 tempUpdates=1 "
                  "traceTiBackboneHops=0 traceTdBackboneHops=0 ")
        for fields, expected in (
            ("temporaryFibInterestTransmissions=2 flowTableFibInterestTransmissions=5",
             {"temporary_fib_interest_transmissions": 2, "flow_table_fib_interest_transmissions": 5}),
            ("temporaryFibForwards=2 flowTableFibForwards=7",
             {"legacy_temporary_fib_matches": 2, "legacy_interest_transmissions": 7}),
        ):
            with self.subTest(fields=fields):
                metrics, _, issues = simulation(base + common + fields + "\n[PATH_FIB_TOTAL] allOk=1\n")
                self.assertEqual(issues, [])
                for field, value in expected.items():
                    self.assertEqual(metrics[field], value)


if __name__ == "__main__":
    unittest.main()
