#!/usr/bin/env python3
"""Compares two measurement files and says what moved.

Reads both shapes the project writes: the flat list of records the benchmark
harness emits, and the document the scenario harness emits, which also carries
the conditions a run was taken under and the scenarios a build could not be
asked at all.

The rule this exists to enforce: a number is only compared when both runs
measured the same thing. A metric present in one file and absent from the other
is reported as absent rather than dropped, a metric the harness marked as not
comparable is printed without a verdict, and conditions that differ between the
runs are called out above the table. Behaviour changes and speed changes look
identical in a column of numbers, and this is where they are kept apart.
"""

import argparse
import json
import math
import sys


def load(path):
    """A file's measurements keyed by metric name, and its document.

    @return (rows, document); `document` is None for the flat legacy files.
    """
    with open(path) as handle:
        loaded = json.load(handle)
    if isinstance(loaded, list):
        return ({row["metric"]: row for row in loaded}, None)

    return ({row["metric"]: row for row in loaded.get("results", [])}, loaded)


def _summary(document):
    if document is None:
        return None
    conditions = document.get("conditions", {})
    machine = conditions.get("machine", {})
    power = conditions.get("power", {})
    content = conditions.get("content", {})
    application = conditions.get("application", {})
    displays = conditions.get("displays", [])

    return {
        "machine": f"{machine.get('name', '?')} - {machine.get('cpu', '?')}, {machine.get('logical_cores', '?')} cores",
        "os": machine.get("os", "?"),
        "power": f"{power.get('source', '?')}, battery {power.get('battery_percent', '?')}%, "
                 f"charging={power.get('charging', '?')}, cpu_limit={power.get('cpu_speed_limit', '?')}",
        "displays": ", ".join(f"{int(one['points'][0])}x{int(one['points'][1])} at {one['scale']}x"
                              for one in displays),
        "region": "x".join(str(value) for value in document.get("layout", {}).get("region_pixels", [])) + " pixels",
        "content": json.dumps({key: content.get(key) for key in
                               ("kind", "patterns", "photographs", "degraded")}, sort_keys=True),
        "build": f"{application.get('version', '?')} {application.get('binary_sha256', '?')} "
                 f"[{document.get('profile', {}).get('name', '?')}]",
    }


# The build is expected to differ - that is usually the point of the comparison.
# Everything else differing means the two runs are not like for like.
_CONDITION_KEYS = ("machine", "os", "power", "displays", "region", "content", "build")


def report_conditions(baseline_document, current_document):
    """Print both runs' conditions; return the ones that differ."""
    before = _summary(baseline_document)
    after = _summary(current_document)
    if before is None and after is None:
        return []
    mismatched = []
    print("conditions")
    for key in _CONDITION_KEYS:
        left = (before or {}).get(key, "-")
        right = (after or {}).get(key, "-")
        differs = left != right and key != "build"
        if differs:
            mismatched.append(key)
        print(f"  {key:<9} {left}")
        print(f"  {'':<9} {right}{'   <- DIFFERS' if differs else ''}")
    print()

    return mismatched


def compare(baseline, current, threshold, condition_reason=""):
    """Print every metric; return whether anything regressed."""
    regressed = False
    for metric in sorted(baseline):
        if metric not in current:
            print(f"  {metric}: absent from the current run")
            continue
        before, after = baseline[metric], current[metric]
        unit = after.get("unit", "ns")
        left, right = float(before["value"]), float(after["value"])
        delta = (right - left) / abs(left) * 100.0 if left else (math.copysign(math.inf, right) if right else 0.0)
        direction = after.get("direction", "lower")
        note = ""
        if condition_reason:
            note = "   NOT COMPARABLE: " + condition_reason
        elif before.get("unit", "ns") != unit:
            note = "   NOT COMPARABLE: measurement units differ"
        elif before.get("direction", "lower") != direction:
            note = "   NOT COMPARABLE: metric direction differs"
        elif not math.isfinite(left) or not math.isfinite(right):
            note = "   NOT COMPARABLE: measurement is not finite"
        elif not after.get("comparable", True) or not before.get("comparable", True):
            note = "   NOT COMPARABLE: " + (after.get("incomparable_reason")
                                            or "the two runs measured different situations")
        elif direction == "none":
            note = "   (informational)"
        elif (-delta if direction == "higher" else delta) > threshold:
            note, regressed = "   REGRESSED", True
        print(f"  {metric}: {left:.4g} -> {right:.4g} {unit} ({delta:+.1f}%){note}")
    for metric in sorted(current):
        if metric not in baseline:
            print(f"  {metric}: absent from the baseline run")

    return regressed


def report_list(document, key, label, format_entry):
    entries = (document or {}).get(key, [])
    if not entries:
        return
    print(f"\n{label}")
    for entry in entries:
        print(f"  {format_entry(entry)}")


def main():
    parser = argparse.ArgumentParser(description="Compare two benchmark or scenario result files.")
    parser.add_argument("baseline")
    parser.add_argument("current")
    parser.add_argument("--threshold", type=float, default=15.0,
                        help="regression threshold in percent (default 15)")
    arguments = parser.parse_args()

    baseline, baseline_document = load(arguments.baseline)
    current, current_document = load(arguments.current)

    mismatched = report_conditions(baseline_document, current_document)
    print("measurements")
    reason = "conditions differ: " + ", ".join(mismatched) if mismatched else ""
    regressed = compare(baseline, current, arguments.threshold, reason)
    for document, label in ((baseline_document, "baseline"), (current_document, "current")):
        report_list(document, "absent", f"not run by the {label} build",
                    lambda entry: f"{entry['scenario']}/{entry['stack']}: {entry['reason']}")
        report_list(document, "warnings", f"warnings from the {label} run", str)
    if mismatched:
        print(f"\nthe runs differ in {', '.join(mismatched)}, so the table above is not like for like")

    return 1 if regressed else 0


if __name__ == "__main__":
    sys.exit(main())
