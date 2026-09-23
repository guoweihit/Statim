#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Analyze recorded experiment parameters with NumPy and SciPy (Python 3.10+).

--run-root supplies a completed run. --output names the report JSON, and its
suffix-free sibling directory receives measurements, statistics and issue
reports. --verify also checks recorded file digests and occupancy trajectories.
"""
import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent / "analysis"))
from dataset import load_reference, load_run, save_analysis


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True, help="Recorded experiment run directory")
    parser.add_argument("--output", type=Path, required=True, help="New report JSON path")
    parser.add_argument("--tables", type=Path, help="New table directory; default: output path without suffix")
    parser.add_argument("--confidence", type=float, default=0.95, help="Two-sided confidence level (default: 0.95)")
    parser.add_argument("--verify", action="store_true", help="Recompute digests and individual occupancy residence samples")
    parser.add_argument("--data-root", type=Path, help="Optional historical run directory or observations JSON")
    args = parser.parse_args(argv)
    root = args.run_root.resolve()
    output = args.output.resolve()
    tables = args.tables.resolve() if args.tables else output.with_suffix("")
    if not 0 < args.confidence < 1:
        parser.error("--confidence must lie strictly between zero and one")
    code = Path(__file__).resolve().parents[1]
    if output.is_relative_to(code) or tables.is_relative_to(code):
        parser.error("Select analysis output paths outside the source tree")
    if output == tables or output.is_relative_to(tables):
        parser.error("Choose separate report and table paths")
    if output.exists() or (tables.exists() and (not tables.is_dir() or any(tables.iterdir()))):
        parser.error("Choose a new report and an empty table directory")
    if args.data_root:
        reference = args.data_root.resolve()
        if output.is_relative_to(reference) or tables.is_relative_to(reference):
            parser.error("Choose output paths outside the historical input")
    try:
        data = load_run(root, verify=args.verify)
        historical = load_reference(args.data_root) if args.data_root else None
        report = save_analysis(data, tables, output, args.confidence, historical)
    except (OSError, ValueError, KeyError) as exc:
        parser.exit(2, "error: {}\n".format(exc))
    print(json.dumps({name: report[name] for name in
        ("status", "process_count", "loaded_processes", "errors", "warnings", "tables")}, indent=2))
    return 1 if report["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
