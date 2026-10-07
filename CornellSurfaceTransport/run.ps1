param(
    [int]$Size = 96,
    [int]$Grid = 24,
    [int]$Samples = 256,
    [int]$Orders = 3,
    [int]$ReferenceSpp = 1024,
    [int]$DisplaySpp = 128,
    [int[]]$Seeds = @(42, 43, 44)
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$phantomRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildDir = Join-Path $phantomRoot 'build/windows-release'
$resultDir = Join-Path $repoRoot 'scratch/cornell-results'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -property installationPath
if (!$vsRoot) { throw 'Visual Studio installation not found.' }
$vcvars = Join-Path $vsRoot 'VC/Auxiliary/Build/vcvars64.bat'
$ninja = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (!(Test-Path -LiteralPath $ninja)) { throw 'Visual Studio Ninja not found.' }
$command = '"{0}" >nul && cmake -S "{1}" -B "{2}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="{3}" && cmake --build "{2}" --target CornellSurfaceTransport' -f $vcvars, $phantomRoot, $buildDir, $ninja
& cmd /c $command
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& ctest --test-dir $buildDir -R '^CornellSurfaceTransport\.' --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Checks failed.' }
$exe = Join-Path $buildDir 'RayTracer/CornellSurfaceTransport.exe'
$rows = @()
foreach ($seed in $Seeds) {
    foreach ($reduce in @(1, 0)) {
        $name = if ($reduce) { "reduced$seed" } else { "full$seed" }
        $destination = Join-Path $resultDir $name
        & $exe $destination $Size $Grid $Samples $Orders $ReferenceSpp $seed $reduce $DisplaySpp
        if ($LASTEXITCODE -ne 0) { throw "Experiment failed: $name" }
        $rows += Import-Csv -LiteralPath (Join-Path $destination 'metrics.csv')
    }
}
$rows | Export-Csv -LiteralPath (Join-Path $resultDir 'summary.csv') -NoTypeInformation -Encoding UTF8
$rows | Format-Table seed, reduce, solve_seconds, render_seconds, reference_seconds, diffuse_relative_l2
Write-Output "Results: $resultDir"
