#!/usr/bin/env python3
import argparse
import json
import sys


def load(path):
    with open(path) as handle:
        return {row["metric"]: float(row["value"]) for row in json.load(handle)}


def main():
    parser = argparse.ArgumentParser(description="Compare two benchmark result files.")
    parser.add_argument("baseline")
    parser.add_argument("current")
    parser.add_argument("--threshold", type=float, default=15.0,
                        help="regression threshold in percent (default 15)")
    args = parser.parse_args()

    baseline = load(args.baseline)
    current = load(args.current)

    regressed = False
    for metric in sorted(baseline):
        if metric not in current:
            print(f"{metric}: missing in current")
            continue
        before = baseline[metric]
        after = current[metric]
        delta = (after - before) / before * 100.0 if before else 0.0
        flag = ""
        if delta > args.threshold:
            flag = " REGRESSED"
            regressed = True
        print(f"{metric}: {before:.1f} -> {after:.1f} ns ({delta:+.1f}%){flag}")

    for metric in sorted(current):
        if metric not in baseline:
            print(f"{metric}: new")

    return 1 if regressed else 0


if __name__ == "__main__":
    sys.exit(main())
