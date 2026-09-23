#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare complete UTF-8 log lines and retain every observed difference.

Byte equality and text equality are reported separately. The scientific table
comparisons assess the measured values associated with each experiment.
"""
import argparse
import difflib
import json
import tarfile
from pathlib import Path, PurePosixPath

def compare_payloads(current, historical):
    left = historical.decode("utf-8").splitlines()
    right = current.decode("utf-8").splitlines()
    differences = []
    for op, a, b, c, d in difflib.SequenceMatcher(a=left, b=right, autojunk=False).get_opcodes():
        if op != "equal":
            differences.append(
                {
                    "operation": op,
                    "historical_start_line": a + 1,
                    "current_start_line": c + 1,
                    "historical": left[a:b],
                    "current": right[c:d],
                }
            )
    return {
        "raw_byte_identical": current == historical,
        "text_equal": not differences,
        "historical_line_count": len(left),
        "current_line_count": len(right),
        "differences": differences,
    }


def reference_logs(data_root, family):
    """Select expanded reference logs, falling back to a legacy archive.

    Recent reruns keep stdout/stderr in ordinary directories. Earlier datasets
    package stderr logs in archives. Prefer the expanded logs when both exist;
    a selected source provides every historical comparison input.
    """
    directory = Path(data_root) / "raw" / family
    archive = Path(data_root) / "raw" / (family + "-logs.tar.gz")
    if directory.is_dir():
        return directory
    if archive.is_file():
        return archive
    raise FileNotFoundError("Historical logs not found: {} or {}".format(directory, archive))


def compare_family(fresh_directory, historical_source):
    historical_source = Path(historical_source)
    reference = {}
    if historical_source.is_dir():
        reference = {path.name: path.read_bytes() for path in historical_source.glob("*.err")}
    elif historical_source.is_file():
        with tarfile.open(historical_source, "r:*") as archive:
            for member in archive:
                if (
                    not member.isfile()
                    or PurePosixPath(member.name).name != member.name
                    or member.name in reference
                ):
                    raise ValueError("Unexpected historical log member: " + member.name)
                reference[member.name] = archive.extractfile(member).read()
    else:
        raise FileNotFoundError("Historical logs not found: " + str(historical_source))
    if not reference:
        raise ValueError("Historical log source is empty: " + str(historical_source))
    current = {path.name: path for path in fresh_directory.glob("*.err")}
    result = {
        "historical_source": str(historical_source),
        "historical_layout": "directory" if historical_source.is_dir() else "archive",
        "missing_current_logs": sorted(set(reference) - set(current)),
        "unexpected_current_logs": sorted(set(current) - set(reference)),
        "compared_logs": 0,
        "byte_identical_logs": 0,
        "line_ending_only_logs": 0,
        "different_logs": [],
    }
    for name in sorted(set(current) & set(reference)):
        compared = compare_payloads(current[name].read_bytes(), reference[name])
        result["compared_logs"] += 1
        if compared["raw_byte_identical"]:
            result["byte_identical_logs"] += 1
        elif compared["text_equal"]:
            result["line_ending_only_logs"] += 1
        else:
            result["different_logs"].append({"name": name, **compared})
    result["text_equal"] = not (
        result["missing_current_logs"]
        or result["unexpected_current_logs"]
        or result["different_logs"]
    )
    result["comparison_unit"] = "complete UTF-8 lines"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", required=True, type=Path)
    parser.add_argument(
        "--data-root", required=True, type=Path, help="External historical reference data"
    )
    parser.add_argument("--family", required=True, choices=["primary", "paths"])
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    base, output = args.data_root.resolve(), args.output.resolve()
    if output.is_relative_to(base) or output.is_relative_to(Path(__file__).resolve().parents[1]):
        parser.error("Comparison report must be outside historical data and code.")
    if output.exists():
        parser.error("Refusing to overwrite a comparison report.")
    result = compare_family(
        args.run_root / "data/raw" / args.family, reference_logs(base, args.family)
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "logs": result["compared_logs"],
                "text_equal": result["text_equal"],
                "different_logs": len(result["different_logs"]),
                "report": str(output),
            }
        )
    )
    return 0 if result["text_equal"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
