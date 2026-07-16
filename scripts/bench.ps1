$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build-bench"

$machine = if ($env:BENCH_MACHINE) { $env:BENCH_MACHINE } else { $env:COMPUTERNAME }
$commit = (git -C $repoRoot rev-parse --short HEAD).Trim()
$os = "$([System.Environment]::OSVersion.Platform) $([System.Environment]::OSVersion.Version)"

cmake -S $repoRoot -B $buildDir -G Ninja `
    -DSIDESCOPES_BENCH=ON -DSIDESCOPES_BUILD_TESTS=OFF | Out-Null
cmake --build $buildDir --target sidescopes_bench | Out-Null

$resultsDir = Join-Path $repoRoot "bench-results"
New-Item -ItemType Directory -Force -Path $resultsDir | Out-Null
$xmlOut = Join-Path $buildDir "bench.xml"
& (Join-Path $buildDir "bench/sidescopes_bench.exe") --reporter XML --out $xmlOut | Out-Null

$outJson = Join-Path $resultsDir "$machine-$commit.json"
python3 - $xmlOut $machine $os $commit $outJson @'
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
'@

Write-Output $outJson
