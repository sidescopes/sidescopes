#!/bin/sh
# Drives a built SideScopes through named scenarios and records what each one
# costs, so that two machines - or two versions - can be compared on the same
# interactions rather than on a description of them.
#
#   scripts/app-scenarios.sh --list
#   scripts/app-scenarios.sh                                   # every scenario
#   scripts/app-scenarios.sh --scenarios idle-region,region-scan --stacks WVR
#   scripts/app-scenarios.sh --app <another build> --out old.json
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
# It synthesises pointer and keyboard events, which each system has its own
# conditions for: macOS needs the ACCESSIBILITY permission for whichever
# application runs this - System Settings > Privacy & Security > Accessibility -
# and Linux needs an X session, because a Wayland compositor discards
# synthesised events without saying so. The harness probes before it starts and
# says which it is.
#
# Everything system-specific is in a platform module and a content window:
# scripts/scenarios/quartz.py with content_window.m on macOS, x11.py with
# content_window.c on Linux, chosen by desktop.py. A Windows port adds a third
# pair and changes nothing else.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

PYTHONPATH="$repo_root${PYTHONPATH:+:$PYTHONPATH}" exec python3 -m scripts.scenarios.run "$@"
