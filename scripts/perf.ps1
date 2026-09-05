# Builds and runs the performance harness, writing one JSON result file that
# scripts/bench-compare.py can diff against a stored baseline. Any argument is
# forwarded to the harness.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build-bench"

$machine = if ($env:BENCH_MACHINE) { $env:BENCH_MACHINE } else { $env:COMPUTERNAME }
$commit = (git -C $repoRoot rev-parse --short HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw "Cannot identify the measured revision" }
$os = "$([System.Environment]::OSVersion.Platform) $([System.Environment]::OSVersion.Version)"

# Release, like the shipping build: a debug build measures the compiler.
cmake -S $repoRoot -B $buildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Release -DSIDESCOPES_BENCH=ON -DSIDESCOPES_BUILD_TESTS=OFF | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Benchmark configuration failed" }
cmake --build $buildDir --target sidescopes_perf | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Performance harness build failed" }

$resultsDir = Join-Path $repoRoot "bench-results"
New-Item -ItemType Directory -Force -Path $resultsDir | Out-Null
$outJson = Join-Path $resultsDir "perf-$machine-$commit.json"

& (Join-Path $buildDir "bench/sidescopes_perf.exe") `
    --machine $machine --os $os --commit $commit --out $outJson @args
if ($LASTEXITCODE -ne 0) { throw "Performance harness run failed" }

Write-Output $outJson
