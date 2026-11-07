# Builds manager, independent NVIDIA NR and retained DLSS/FSR/XeSS scalers. Nothing is deployed.
# -Only host,neural,scalers selects products. CMake and Visual Studio C++ tools must be on PATH.
# -IncludeTools also builds the UI preview and offline upscaler diagnostics.
param(
    [string[]]$Only = @(), [switch]$IncludeTools,
    [string]$HostBuildDir = 'manager\build',
    [string]$NeuralBuildDir = 'addons\NeuralRendering\build',
    [string]$ScalerBuildDir = 'addons\Upscalers\build',
    [ValidateRange(1, 256)][int]$Jobs = 2
)
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split "," } | Where-Object { $_ })   # accepts -Only a,b when started with -File
$root = Split-Path $PSScriptRoot -Parent   # the repository folder
function BuildPath([string]$path) {
    if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $root $path }
    [IO.Path]::GetFullPath($path)
}
$hostTargets = @('Lossless')
if ($IncludeTools) { $hostTargets += 'eam_uipreview' }
$scalerArgs = @('--target', 'DLSS4DLAA', 'FSR3UPSC')
if (Test-Path "$root\addons\Upscalers\external\xess\inc\xess\xess_d3d12.h") { $scalerArgs += 'XESSUPSC' }
if ($IncludeTools) { $scalerArgs = @() }
$targets = @(
    @{ Name = 'host'; Source = "$root\manager"; Dir = (BuildPath $HostBuildDir); Args = @('--target') + $hostTargets },
    @{ Name = 'neural'; Source = "$root\addons\NeuralRendering"; Dir = (BuildPath $NeuralBuildDir); Args = @('--target','DLSSNR02') },
    @{ Name = 'scalers'; Source = "$root\addons\Upscalers"; Dir = (BuildPath $ScalerBuildDir); Args = $scalerArgs }
)
$failed = 0
foreach ($t in $targets) {
    if ($Only.Count -and ($Only -notcontains $t.Name)) { continue }
    if (-not (Test-Path "$($t.Dir)\CMakeCache.txt")) {   # first time: configure (fetches Dear ImGui and MinHook at pinned versions)
        Write-Host "[$($t.Name)] configuring"
        # Keep native stderr separate: Windows PowerShell 5.1 turns redirected
        # CMake warnings into terminating errors with ErrorActionPreference=Stop.
        & cmake -S $t.Source -B $t.Dir -G 'Visual Studio 17 2022' -A x64
        if ($LASTEXITCODE -ne 0) { $failed++; continue }
    }
    $sourceEntry = Select-String -LiteralPath "$($t.Dir)\CMakeCache.txt" -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$'
    if (-not $sourceEntry -or (BuildPath $sourceEntry.Matches[0].Groups[1].Value) -ne (BuildPath $t.Source)) {
        throw "Build directory $($t.Dir) does not belong to $($t.Source)."
    }
    Write-Host "== $($t.Name)"
    & cmake --build $t.Dir --config Release --parallel $Jobs @($t.Args) -- /nr:false
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
if ($failed) { Write-Host "$failed target(s) failed"; exit 1 } else { Write-Host 'all built'; exit 0 }
