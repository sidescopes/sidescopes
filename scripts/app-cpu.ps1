# Measures what a running SideScopes costs, in cores, over a window - the
# whole-app counterpart to scripts/perf.ps1. Run it once with the scoped
# content STATIC (the idle number) and once with it MOVING (the active one).
#
#   scripts/app-cpu.ps1             # 30 s over the running SideScopes
#   scripts/app-cpu.ps1 60          # a longer window
param(
    [double]$Seconds = 30,
    [int]$ProcessId = 0
)
$ErrorActionPreference = "Stop"

$process = if ($ProcessId -ne 0) {
    Get-Process -Id $ProcessId
} else {
    Get-Process -Name SideScopes -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (-not $process) {
    Write-Error "app-cpu: no SideScopes process found - start it first"
}

$before = $process.TotalProcessorTime.TotalSeconds
$started = Get-Date
Start-Sleep -Seconds $Seconds
$process.Refresh()
$spent = $process.TotalProcessorTime.TotalSeconds - $before
$elapsed = ((Get-Date) - $started).TotalSeconds

$cores = $spent / $elapsed
Write-Output ("pid {0}: {1:N2} CPU-seconds over {2:N1} s = {3:N3} cores ({4:N1}% of one core)" -f `
    $process.Id, $spent, $elapsed, $cores, ($cores * 100))
