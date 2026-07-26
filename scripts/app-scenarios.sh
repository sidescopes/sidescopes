#!/bin/sh
# Drives a built SideScopes through named scenarios and records what each one
# costs, so that two machines - or two versions - can be compared on the same
# interactions rather than on a description of them.
#
#   scripts/app-scenarios.sh --list
#   scripts/app-scenarios.sh                                   # every scenario
#   scripts/app-scenarios.sh --scenarios idle-region,region-scan --stacks WVR
#   scripts/app-scenarios.sh --app /path/to/other/SideScopes.app --out old.json
#
# Then compare two result files:
#
#   scripts/bench-compare.py old.json new.json
#
# This is the counterpart to scripts/perf.sh, which measures the analysis
# pipeline on synthetic frames with no application running. Here the real
# application is launched, driven through the pointer and the keyboard, and
# measured as the machine feels it.
#
# macOS only for now. It needs the ACCESSIBILITY permission for whichever
# application runs it - System Settings > Privacy & Security > Accessibility -
# because it synthesises pointer and keyboard events. A Windows port would
# replace scripts/scenarios/quartz.py and content_window.m and nothing else.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

PYTHONPATH="$repo_root${PYTHONPATH:+:$PYTHONPATH}" exec python3 -m scripts.scenarios.run "$@"
