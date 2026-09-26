# run_raytracer_scenarios.ps1 - Run all RayTracerView scenario tests
# Usage: .\RayTracer\RayTracerView\run_raytracer_scenarios.ps1 [-Configuration Debug|Release]
# Run from the repository root or the RayTracer\RayTracerView directory.

param(
    [string]$Configuration = "Debug"
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$preset    = "windows-$($Configuration.ToLower())"
# RayTracerView.exe is built from the CMake source dir below. Phantom and CGLib each have their own
# CMakePresets.json whose build\<preset>\ mirrors the source tree, so try every enclosing
# preset root (innermost first) and use the first one that has built it.
$srcDir    = [System.IO.Path]::GetFullPath((Join-Path $scriptDir ".."))
$exe       = $null
$tried     = @()
for ($root = $srcDir; $root; $root = Split-Path -Parent $root) {
    if (-not (Test-Path (Join-Path $root "CMakePresets.json"))) { continue }
    $rel       = $srcDir.Substring($root.Length).TrimStart('\')
    $candidate = (@($root, "build", $preset, $rel, "RayTracerView.exe") | Where-Object { $_ }) -join '\'
    $tried    += $candidate
    if (Test-Path $candidate) { $exe = $candidate; break }
}
$scenDir = Join-Path $scriptDir "scenarios"

if (-not $exe) {
    Write-Host "ERROR: Executable not found. Tried:"
    $tried | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "Using $exe"

$scenarios = Get-ChildItem "$scenDir\*.json" | Sort-Object Name
if ($scenarios.Count -eq 0) {
    Write-Host "ERROR: No scenario JSON files found in $scenDir"
    exit 1
}

# Ensure screenshots output directory exists
$screenshotsDir = Join-Path $scenDir "screenshots"
New-Item -ItemType Directory -Force $screenshotsDir | Out-Null

$passed = 0
$failed = 0

foreach ($s in $scenarios) {
    Write-Host "Running: $($s.BaseName)"
    $proc = Start-Process -FilePath $exe `
        -ArgumentList "--run-scenario `"$($s.FullName)`"" `
        -PassThru -Wait -WorkingDirectory $scriptDir
    if ($proc.ExitCode -eq 0) {
        Write-Host "PASSED: $($s.BaseName)"
        $passed++
    } else {
        Write-Host "FAILED: $($s.BaseName) (exit $($proc.ExitCode))"
        $failed++
    }
}

Write-Host ""
Write-Host "Results: $passed/$($scenarios.Count) PASSED, $failed FAILED"
exit $failed
