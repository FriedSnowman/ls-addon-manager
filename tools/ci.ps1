# Fast CPU-only smoke tests: installer payload validation and NR runtime loading.
# The default builds no GUI, shaders, GPU tests, or sample addon and downloads no dependencies.
# Use -Full for the broader manager, installer, NR and sample build/test suite.
#   powershell -File tools\ci.ps1 [-Full]
param([switch]$Full)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$failed = @()
$logs = Join-Path $root 'build\ci-logs'
New-Item -ItemType Directory -Force $logs | Out-Null

function Section($text) { Write-Host ""; Write-Host "=== $text" }
function Configure($src, $build, $extra = @()) {
    if (Test-Path "$build\CMakeCache.txt") { return }
    & cmake -S $src -B $build -G 'Visual Studio 17 2022' -A x64 @extra
    if ($LASTEXITCODE -ne 0) { throw "cmake could not configure $src" }
}
function Build($build, $targets) {
    & cmake --build $build --config Release --parallel 2 --target @targets -- /nologo /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "the build failed in $build" }
}
function Run($name, $exe, $testArgs = @()) {
    Write-Host "--- $name"
    $testTimer = [Diagnostics.Stopwatch]::StartNew()
    $logBase = Join-Path $logs ($name -replace '[^a-zA-Z0-9_-]', '-')
    $start = @{
        FilePath = $exe; PassThru = $true; WindowStyle = 'Hidden'
        RedirectStandardOutput = "$logBase.out.log"; RedirectStandardError = "$logBase.err.log"
    }
    if ($testArgs.Count) { $start.ArgumentList = @($testArgs | ForEach-Object { '"' + $_ + '"' }) }
    $process = Start-Process @start
    # Windows PowerShell otherwise loses ExitCode when a short-lived process exits.
    $null = $process.Handle
    $limit = if ($Full) { 300 } else { 30 }
    if (-not $process.WaitForExit($limit * 1000)) {
        & taskkill.exe /PID $process.Id /T /F | Out-Null
        throw "$name exceeded its $limit second timeout. Logs: $logBase.*.log"
    }
    $process.WaitForExit()
    Get-Content "$logBase.out.log", "$logBase.err.log" | ForEach-Object { Write-Host $_ }
    Write-Host ('{0}: {1:N2}s' -f $name, $testTimer.Elapsed.TotalSeconds)
    if ($process.ExitCode -ne 0) { throw "FAILED: $name (exit $($process.ExitCode))" }
}

if (-not $Full) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    # Separate build directories keep NEURAL_BUILD_ADDON=OFF out of product builds.
    $smoke = Join-Path $root 'build\ci-smoke'
    Section 'installer payload smoke test'
    Configure "$root\installer" "$smoke\installer"
    Build "$smoke\installer" @('setup_payload_test')
    Run 'installer payload' "$smoke\installer\Release\setup_payload_test.exe"

    Section 'NR runtime loader smoke test'
    Configure "$root\addons\NeuralRendering" "$smoke\neural" @('-DNEURAL_BUILD_ADDON=OFF')
    Build "$smoke\neural" @('neural_forwardertest')
    Run 'NR runtime loader' "$smoke\neural\Release\neural_forwardertest.exe"
    Write-Host ('FAST CI TESTS PASSED ({0:N1}s including build)' -f $timer.Elapsed.TotalSeconds)
    exit 0
}

Section 'the manager and its tests'
Configure "$root\manager" "$root\manager\build"
Build "$root\manager\build" @('Lossless', 'eam_installtest', 'eam_coretest', 'eam_sampletest', 'eam_updatetest', 'eam_updateuitest')
Run 'addon install' "$root\manager\build\Release\eam_installtest.exe"
Run 'addon handling' "$root\manager\build\Release\eam_coretest.exe"
Run 'sample addon' "$root\manager\build\Release\eam_sampletest.exe"
Run 'update check (offline)' "$root\manager\build\Release\eam_updatetest.exe"
Run 'update consent and settings (offline)' "$root\manager\build\Release\eam_updateuitest.exe" @("$logs\update-ui")

Section 'the installer'
Configure "$root\installer" "$root\installer\build"
Build "$root\installer\build" @('setup_core', 'setup_cli', 'setup_test', 'setup_payload_test', 'pack_payload', 'LSAddonManagerSetup')
Run 'installer core (fake Lossless Scaling folders)' "$root\installer\build\Release\setup_test.exe"
Run 'installer file bundle' "$root\installer\build\Release\setup_payload_test.exe"
Run 'Setup exe, silent mode' 'powershell' @('-NoProfile', '-File', "$root\installer\tests\setup_exe_test.ps1", '-Root', $root, '-NoWindow')

Section 'the independent NR addon (no proprietary model or SDK needed)'
Configure "$root\addons\NeuralRendering" "$root\addons\NeuralRendering\build"
Build "$root\addons\NeuralRendering\build" @('DLSSNR02','neural_statustest','neural_forwardertest','neural_flowtest','neural_compositiontest','neural_modeltest','neural_pipelinetest','neural_multipasstest','neural_picturetest')
Run 'NR blocked capture diagnostics' "$root\addons\NeuralRendering\build\Release\neural_statustest.exe"
Run 'NR runtime compatibility and model selection' "$root\addons\NeuralRendering\build\Release\neural_forwardertest.exe"

Section 'the sample addon, built as its README says'
Configure "$root\examples\SampleAddon" "$root\examples\SampleAddon\build"
Build "$root\examples\SampleAddon\build" @('SampleAddon')
if (-not (Test-Path "$root\examples\SampleAddon\build\Release\SampleAddon.dll")) { $failed += 'sample addon standalone build' }

Write-Host ""
if ($failed.Count) { Write-Host "FAILED: $($failed -join '; ')"; exit 1 }
Write-Host 'ALL CI TESTS PASSED'
