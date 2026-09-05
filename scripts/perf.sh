#!/bin/sh
# Builds and runs the performance harness, writing one JSON result file that
# scripts/bench-compare.py can diff against a stored baseline.
#
#   scripts/perf.sh                  # the whole sweep
#   scripts/perf.sh --tiers engine   # one tier, for a quick before/after
#
# Any argument is forwarded to the harness; --machine, --os and --commit are
# filled in here unless the caller overrides them.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$repo_root/build-bench"

machine="${BENCH_MACHINE:-$(hostname -s)}"
commit=$(git -C "$repo_root" rev-parse --short HEAD)
os=$(uname -sr)

# Release, like the shipping build: a debug build measures the compiler.
cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DSIDESCOPES_BENCH=ON -DSIDESCOPES_BUILD_TESTS=OFF >/dev/null
cmake --build "$build_dir" --target sidescopes_perf >/dev/null

results_dir="$repo_root/bench-results"
mkdir -p "$results_dir"
out_json="$results_dir/perf-$machine-$commit.json"
previous_argument=""
for argument do
    [ "$previous_argument" != --out ] || out_json="$argument"
    previous_argument="$argument"
done

"$build_dir/bench/sidescopes_perf" \
    --machine "$machine" --os "$os" --commit "$commit" --out "$out_json" "$@"

echo "$out_json"
