param(
    [int[]]$Sizes = @(96, 256),
    [int[]]$Seeds = @(42, 43, 44),
    [int]$PhotonCount = 200000,
    [double]$Radius = 0.12,
    [int]$DirectSamples = 32,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$phantomRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildDir = Join-Path $phantomRoot 'build/windows-release'
$resultDir = Join-Path $repoRoot 'scratch/cornell-photon-splat'
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
foreach ($size in $Sizes) {
    foreach ($seed in $Seeds) {
        $destination = Join-Path $resultDir "size$size-seed$seed"
        $radiusText = $Radius.ToString([System.Globalization.CultureInfo]::InvariantCulture)
        & $exe --photon-splat $destination $size $PhotonCount $radiusText $DirectSamples $seed
        if ($LASTEXITCODE -ne 0) { throw "Comparison failed: size=$size seed=$seed" }
        $rows += Import-Csv -LiteralPath (Join-Path $destination 'splat_metrics.csv')
    }
}
$rows | Export-Csv -LiteralPath (Join-Path $resultDir 'summary.csv') -NoTypeInformation -Encoding UTF8
$rows | Format-Table size, seed, gpu_wall_seconds, splat_gpu_ms, kdtree_gather_seconds, indirect_relative_l2
Write-Output "Results: $resultDir"
