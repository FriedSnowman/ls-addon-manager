# Builds manager, independent NVIDIA NR and retained DLSS/FSR/XeSS scalers. Nothing is deployed.
# -Only host,neural,scalers selects products. CMake and Visual Studio C++ tools must be on PATH.
# -IncludeTools also builds the UI preview and offline upscaler diagnostics.
param([string[]]$Only = @(), [switch]$IncludeTools)
$Only = @($Only | ForEach-Object { $_ -split "," } | Where-Object { $_ })   # accepts -Only a,b when started with -File
$root = Split-Path $PSScriptRoot -Parent   # the repository folder
$hostTargets = @('Lossless')
if ($IncludeTools) { $hostTargets += 'eam_uipreview' }
$scalerArgs = @('--target', 'DLSS4DLAA', 'FSR3UPSC')
if (Test-Path "$root\addons\Upscalers\external\xess\inc\xess\xess_d3d12.h") { $scalerArgs += 'XESSUPSC' }
if ($IncludeTools) { $scalerArgs = @() }
$targets = @(
    @{ Name = 'host';     Dir = "$root\manager\build";                 Args = @('--target') + $hostTargets },
    @{ Name = 'neural'; Dir = "$root\addons\NeuralRendering\build"; Args = @('--target','DLSSNR02') },
    @{ Name = 'scalers';       Dir = "$root\addons\Upscalers\build"; Args = $scalerArgs }
)
$failed = 0
foreach ($t in $targets) {
    if ($Only.Count -and ($Only -notcontains $t.Name)) { continue }
    if (-not (Test-Path "$($t.Dir)\CMakeCache.txt")) {   # first time: configure (fetches Dear ImGui and MinHook at pinned versions)
        Write-Host "[$($t.Name)] configuring"
        & cmake -S (Split-Path $t.Dir -Parent) -B $t.Dir -G 'Visual Studio 17 2022' -A x64 2>&1 | Select-String -Pattern 'error|Error' | ForEach-Object { $_.Line }
        if ($LASTEXITCODE -ne 0) { $failed++; continue }
    }
    Write-Host "== $($t.Name)"
    Push-Location $t.Dir
    & cmake --build . --config Release --parallel 2 @($t.Args) 2>&1 | Select-String -Pattern 'error|warning C4|vcxproj ->' | ForEach-Object { $_.Line }
    if ($LASTEXITCODE -ne 0) { $failed++ }
    Pop-Location
}
if ($failed) { Write-Host "$failed target(s) failed"; exit 1 } else { Write-Host 'all built' }
