#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$repo_root/build-bench"

machine="${BENCH_MACHINE:-$(hostname -s)}"
commit=$(git -C "$repo_root" rev-parse --short HEAD)
os=$(uname -sr)

cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DSIDESCOPES_BENCH=ON -DSIDESCOPES_BUILD_TESTS=OFF >/dev/null
cmake --build "$build_dir" --target sidescopes_bench >/dev/null

results_dir="$repo_root/bench-results"
mkdir -p "$results_dir"
xml_out="$build_dir/bench.xml"
"$build_dir/bench/sidescopes_bench" --reporter XML --out "$xml_out" >/dev/null

out_json="$results_dir/$machine-$commit.json"
python3 - "$xml_out" "$machine" "$os" "$commit" "$out_json" <<'PY'
import json, sys, xml.etree.ElementTree as ET

xml_path, machine, os_name, commit, out_path = sys.argv[1:6]
rows = []
for result in ET.parse(xml_path).iter("BenchmarkResults"):
    mean = result.find("mean")
    if mean is None:
        continue
    rows.append({
        "machine": machine,
        "os": os_name,
        "commit": commit,
        "metric": result.get("name"),
        "value": float(mean.get("value")),
        "unit": "ns",
    })
with open(out_path, "w") as handle:
    json.dump(rows, handle, indent=2)
    handle.write("\n")
PY

echo "$out_json"
