param(
    [int]$Size = 96,
    [int[]]$Seeds = @(42, 43, 44),
    [int]$PhotonCount = 50000,
    [double]$Radius = 0.12,
    [int]$DirectSamples = 32,
    [double]$Retention = 0.65,
    [int]$MaxParticles = 10000,
    [int]$Ensembles = 32,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$phantomRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildDir = Join-Path $phantomRoot 'build/windows-release'
$resultDir = Join-Path $repoRoot 'scratch/pbvr-photon'
if (!$SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsRoot = & $vswhere -latest -products '*' -property installationPath
    if (!$vsRoot) { throw 'Visual Studio installation not found.' }
    $vcvars = Join-Path $vsRoot 'VC/Auxiliary/Build/vcvars64.bat'
    $ninja = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
    $command = '"{0}" >nul && cmake -S "{1}" -B "{2}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="{3}" && cmake --build "{2}" --target CornellSurfaceTransport RayTracerTest PhotonSplatGpuTest' -f $vcvars, $phantomRoot, $buildDir, $ninja
    & cmd /c $command
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    foreach ($test in @('RayTracerTest.exe', 'PhotonSplatGpuTest.exe')) {
        & (Join-Path $buildDir "RayTracer/$test")
        if ($LASTEXITCODE -ne 0) { throw "Tests failed: $test" }
    }
}
$rows = @()
$exe = Join-Path $buildDir 'RayTracer/CornellSurfaceTransport.exe'
$culture = [System.Globalization.CultureInfo]::InvariantCulture
foreach ($seed in $Seeds) {
    $destination = Join-Path $resultDir "size$Size-seed$seed-retain$($Retention.ToString($culture))-cap$MaxParticles-e$Ensembles"
    & $exe --photon-pbvr $destination $Size $PhotonCount $Radius.ToString($culture) $DirectSamples $seed $Retention.ToString($culture) $MaxParticles $Ensembles
    if ($LASTEXITCODE -ne 0) { throw "PBVR comparison failed: seed=$seed" }
    $rows += Import-Csv -LiteralPath (Join-Path $destination 'pbvr_metrics.csv')
}
$rows | Export-Csv -LiteralPath (Join-Path $resultDir 'summary.csv') -NoTypeInformation -Encoding UTF8
$rows | Where-Object { $_.mode -ne 'pbvr' -or [int]$_.ensembles -eq $Ensembles } |
    Format-Table mode, emission_seed, ensembles, mean_traced_rays, mean_transport_seconds, indirect_relative_l2, indirect_mean_ratio
Write-Output "Results: $resultDir"
