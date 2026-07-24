#!/bin/sh
# Measures what a running SideScopes costs, in cores, over a window. This is
# the whole-app counterpart to scripts/perf.sh: the harness measures the
# analysis pipeline in isolation, this measures everything - capture thread,
# analysis thread, render loop - as the machine actually feels it.
#
#   scripts/app-cpu.sh              # 30 s over the running SideScopes
#   scripts/app-cpu.sh 60           # a longer window
#   scripts/app-cpu.sh 30 <pid>     # a specific process
#
# Run it twice to answer the question that matters: once with the scoped
# content STATIC (nothing moving on screen - the idle number), once with it
# MOVING (scrub a photo, drag a slider - the active number). Launch the app
# with `open -n build/SideScopes.app` so the screen-recording grant survives.
set -eu

seconds="${1:-30}"
pid="${2:-}"

if [ -z "$pid" ]; then
    pid=$(pgrep -x SideScopes | head -1 || true)
fi
if [ -z "$pid" ]; then
    echo "app-cpu: no SideScopes process found - start it first" >&2
    exit 1
fi

python3 - "$pid" "$seconds" <<'PY'
import subprocess, sys, time


def cpu_seconds(pid):
    # ps reports cumulative CPU as [[dd-]hh:]mm:ss.ss; the trailing fields are
    # always seconds and minutes, so parse from the right.
    out = subprocess.run(["ps", "-p", pid, "-o", "cputime="],
                         capture_output=True, text=True, check=True).stdout.strip()
    if not out:
        raise SystemExit("app-cpu: process went away")
    days, _, rest = out.rpartition("-")
    parts = [float(p) for p in rest.split(":")]
    total = 0.0
    for part in parts:
        total = total * 60.0 + part
    return total + (float(days) * 86400.0 if days else 0.0)


pid, seconds = sys.argv[1], float(sys.argv[2])
before, started = cpu_seconds(pid), time.monotonic()
time.sleep(seconds)
after, elapsed = cpu_seconds(pid), time.monotonic() - started

spent = after - before
print(f"pid {pid}: {spent:.2f} CPU-seconds over {elapsed:.1f} s "
      f"= {spent / elapsed:.3f} cores ({spent / elapsed * 100.0:.1f}% of one core)")
PY
