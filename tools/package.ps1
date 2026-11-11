# Builds dist\LSAddonManager-build-<number>-x64.exe and its SHA-256 checksum.
# The installer carries the manager, addons and documentation; --extract <folder> writes them out for installing by hand.
# The user-provided NR model is never packaged. All four built addon products are required.
# GPL NR corresponding source and its exact ImGui build dependency ship inside the payload.
#   powershell -File tools/package.ps1 -Build <number>
param(
    [Parameter(Mandatory = $true)][ValidateRange(1, 65535)][int]$Build,
    [string]$HostBuildDir = 'manager\build',
    [string]$NeuralBuildDir = 'addons\NeuralRendering\build',
    [string]$ScalerBuildDir = 'addons\Upscalers\build',
    [string]$SetupBuildDir = 'installer\build_setup',
    [ValidateRange(1, 256)][int]$Jobs = 2,
    [ValidatePattern('^([A-Za-z0-9][A-Za-z0-9._-]*)?$')][string]$PackageSuffix = ''
)
$ErrorActionPreference = 'Stop'
function RemoveUnder([string]$Path,[string]$Parent) {
    $absolute=[IO.Path]::GetFullPath($Path)
    $boundary=[IO.Path]::GetFullPath($Parent).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw "Refusing removal outside $boundary : $absolute"}
    if(Test-Path -LiteralPath $absolute){Remove-Item -LiteralPath $absolute -Recurse -Force}
}
$root = Split-Path $PSScriptRoot -Parent
function BuildPath([string]$path) {
    if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $root $path }
    [IO.Path]::GetFullPath($path)
}
$HostBuildDir = BuildPath $HostBuildDir
$NeuralBuildDir = BuildPath $NeuralBuildDir
$ScalerBuildDir = BuildPath $ScalerBuildDir
$SetupBuildDir = BuildPath $SetupBuildDir
$Version = "Build $Build"
$buildSlug = "build-$Build"
if ($PackageSuffix) { $buildSlug += "-$PackageSuffix" }
# Always build locally; downloaded or stale release artifacts are not a fallback.
& "$PSScriptRoot\build_all.ps1" -Build $Build -Only host,neural,scalers -HostBuildDir $HostBuildDir -NeuralBuildDir $NeuralBuildDir -ScalerBuildDir $ScalerBuildDir -Jobs $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Local build failed; refusing to package existing artifacts.' }

$name = "LSAddonManager-$buildSlug-x64"
$dist = "$root\dist"
$work = "$root\build\package-$buildSlug"
$stage = "$work\payload"
RemoveUnder $stage $work
New-Item -ItemType Directory -Force "$stage\addons" | Out-Null

function Need($path, $what) { if (-not (Test-Path $path)) { throw "missing build output: $what ($path)" } }
Need "$HostBuildDir\Release\Lossless.dll" 'the manager'
Copy-Item "$HostBuildDir\Release\Lossless.dll" $stage
Copy-Item "$root\manager\manager-icon.ico", "$root\manager\manager-icon.png" $stage

& "$PSScriptRoot\package_neural_source.ps1" -Destination "$root\build\NeuralRendering-source.zip" -NeuralBuildDir $NeuralBuildDir

$addons = @(
    @{ Id = 'DLSSNR02'; Dir = "$root\addons\NeuralRendering"; Bin = "$NeuralBuildDir\Release";
       Files = @('DLSSNR02.dll','NeuralRendering_nvngx.dll');
       Extra = @{ 'NOTICE.md' = "$root\addons\NeuralRendering\NOTICE.md"; 'README.md' = "$root\addons\NeuralRendering\README.md";
                  'NeuralRendering-source.zip' = "$root\build\NeuralRendering-source.zip" } },
    # DLSS Upscaler (a preview since 0.9.1: its addon.json keeps it off until switched on): built from the same sources; its addon.json lives
    # in products\DLSS4DLAA, and NVIDIA's DLSS runtime ships in its dlss folder
    @{ Id = 'DLSS4DLAA'; Dir = "$root\addons\Upscalers\products\DLSS4DLAA"; Bin = "$ScalerBuildDir\Release"; Files = @('DLSS4DLAA.dll');
       Extra = @{ 'NVIDIA-LICENSE.txt' = "$root\addons\Upscalers\external\ngx\LICENSE.txt"; 'LICENSE.txt' = "$root\addons\Upscalers\LICENSE";
                  'dlss\nvngx_dlss.dll' = "$root\addons\Upscalers\external\ngx\bin\nvngx_dlss.dll" } },
    # FSR Upscaler: the same sources again; AMD's FidelityFX runtime (MIT, signed by AMD) ships in its fsr folder (tools\fetch_ffx_sdk.ps1)
    @{ Id = 'FSR3UPSC'; Dir = "$root\addons\Upscalers\products\FSR3UPSC"; Bin = "$ScalerBuildDir\Release"; Files = @('FSR3UPSC.dll');
       Extra = @{ 'AMD-FidelityFX-LICENSE.txt' = "$root\addons\Upscalers\third_party\ffx\LICENSE.txt"; 'LICENSE.txt' = "$root\addons\Upscalers\LICENSE";
                  'fsr\amd_fidelityfx_dx12.dll' = "$root\addons\Upscalers\external\ffx\bin\amd_fidelityfx_dx12.dll";
                  # the second choice in the manager's Runtimes list: FSR 4.1.1b, OptiScaler's build (tools\fetch_fsr4.ps1)
                  'runtimes\FSR\0dd77d9c\amd_fidelityfx_dx12.dll' = "$root\addons\Upscalers\external\fsr4\amd_fidelityfx_dx12.dll";
                  'runtimes\FSR\0dd77d9c\ABOUT.txt' = "$root\addons\Upscalers\products\FSR3UPSC\FSR4-ABOUT.txt";
                  'runtimes\FSR\0dd77d9c\LICENSE.txt' = "$root\addons\Upscalers\third_party\ffx4\LICENSE.txt" } },
    # XeSS Upscaler: the same sources again; Intel's XeSS runtime (Intel Simplified Software License, signed by Intel) ships in its xess folder,
    # with Intel's licence next to it, as that licence asks (tools\fetch_xess_sdk.ps1)
    @{ Id = 'XESSUPSC'; Dir = "$root\addons\Upscalers\products\XESSUPSC"; Bin = "$ScalerBuildDir\Release"; Files = @('XESSUPSC.dll');
       Extra = @{ 'Intel-XeSS-LICENSE.txt' = "$root\addons\Upscalers\external\xess\LICENSE.txt"; 'LICENSE.txt' = "$root\addons\Upscalers\LICENSE";
                  'xess\libxess.dll' = "$root\addons\Upscalers\external\xess\bin\libxess.dll" } }
)
$included = @()
foreach ($a in $addons) {
    $missing = @($a.Files | Where-Object { -not (Test-Path "$($a.Bin)\$_") })
    if ($missing.Count) { throw "Required addon $($a.Id) is incomplete: $($missing -join ', ')" }
    $dst = "$stage\addons\$($a.Id)"
    New-Item -ItemType Directory -Force $dst | Out-Null
    foreach ($f in $a.Files) { Copy-Item "$($a.Bin)\$f" $dst }
    Copy-Item "$($a.Dir)\addon.json" $dst
    foreach ($icon in 'icon.svg', 'icon.png') { if (Test-Path "$($a.Dir)\$icon") { Copy-Item "$($a.Dir)\$icon" $dst } }
    if (Test-Path "$($a.Dir)\LICENSE") { Copy-Item "$($a.Dir)\LICENSE" "$dst\LICENSE.txt" }
    if ($a.Extra) { foreach ($extra in $a.Extra.Keys) { Need $a.Extra[$extra] "$extra for $($a.Id) (run tools\$(if ($a.Id -eq 'FSR3UPSC') { 'fetch_ffx_sdk.ps1' } elseif ($a.Id -eq 'XESSUPSC') { 'fetch_xess_sdk.ps1' } else { 'fetch_ngx_sdk.ps1' }))"; New-Item -ItemType Directory -Force (Split-Path "$dst\$extra") | Out-Null; Copy-Item $a.Extra[$extra] "$dst\$extra" } }
    $included += $a.Id
}

# Documentation is available through --extract; installation copies the manager, icons and addons only.
$installFiles = @(Get-ChildItem $stage -Recurse -File)
Copy-Item "$root\LICENSE" "$stage\LICENSE.txt"
Copy-Item "$root\NOTICE.md", "$root\DISCLAIMER.md", "$root\CHANGELOG.md" $stage
@"
LS Addon Manager $Version

Close Lossless Scaling before installing. Run $name.exe to install,
repair or remove the manager. Existing files are backed up and failures roll back.
For manual installation, --extract files extracts the payload. Preserve the original
Lossless.dll as Lossless_original.dll before replacing the proxy.

Only locally built addons are included. The retired DLSS5NR01 plugin is ignored by
the manager. Its owned executables move to backups on upgrade; user models and
settings stay at their paths. DLSSNR02 uses fresh settings and NVIDIA hardware only.
No neural model is included. Corresponding NR source and licences are in the addon folder.
See NOTICE.md and DISCLAIMER.md beside this file.
"@ | Set-Content -Encoding UTF8 "$stage\INSTALL.txt"

# The single-file installer: the payload and documentation become a resource of Setup.exe.
# It is built in its own folder (installer\build_setup), so the test build of the installer, which carries no files, stays as the tests expect.
$setupBuild = $SetupBuildDir
$payloadDir = $stage
& cmake -S "$root\installer" -B $setupBuild -G 'Visual Studio 17 2022' -A x64 "-DEAM_BUILD_NUMBER=$Build" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Installer configure failed' }
& cmake --build $setupBuild --config Release --target pack_payload fake_original --parallel $Jobs -- /nr:false | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'the installer packer did not build' }
$bundle = "$work\payload.bin"
& "$setupBuild\Release\pack_payload.exe" $payloadDir $bundle
if ($LASTEXITCODE -ne 0) { throw 'packing the installer files failed' }
& cmake -S "$root\installer" -B $setupBuild "-DSETUP_PAYLOAD=$bundle" "-DEAM_BUILD_NUMBER=$Build" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Installer payload configure failed' }
& cmake --build $setupBuild --config Release --target LSAddonManagerSetup --parallel $Jobs -- /nr:false
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed; refusing stale Setup output.' }
$setupExe = "$setupBuild\Release\LSAddonManagerSetup.exe"
if (-not (Test-Path $setupExe)) { throw 'the Setup exe did not build' }
if ([Diagnostics.FileVersionInfo]::GetVersionInfo($setupExe).FileVersion -ne $Version) {
    throw "The Setup file version does not match $Version."
}

# Check the finished exe as a person would use it: it must report the version it carries, and install into a fake Lossless Scaling folder byte for byte
$check = "$env:TEMP\setup_check_$PID"
RemoveUnder $check $env:TEMP
New-Item -ItemType Directory $check | Out-Null
$vlog = "$check\version.log"
$p = Start-Process $setupExe -ArgumentList '--version', '--no-remember', '--log', "`"$vlog`"" -PassThru -Wait -WindowStyle Hidden
$said = if (Test-Path $vlog) { (Get-Content $vlog -Raw).Trim() } else { '' }
if ($p.ExitCode -ne 0 -or $said -ne "payload $Version") { throw "the Setup exe reports '$said' (exit $($p.ExitCode)), expected 'payload $Version'" }
$fakeOriginal = "$setupBuild\Release\fake_original.dll"
Need $fakeOriginal 'installer test fixture'
$fake = "$check\ls"
New-Item -ItemType Directory $fake | Out-Null
Copy-Item "$env:SystemRoot\System32\cmd.exe" "$fake\LosslessScaling.exe"
Copy-Item $fakeOriginal "$fake\Lossless.dll"
$ilog = "$check\install.log"
$p = Start-Process $setupExe -ArgumentList '--silent', 'install', '--folder', "`"$fake`"", '--no-remember', '--log', "`"$ilog`"" -PassThru -Wait -WindowStyle Hidden
if ($p.ExitCode -ne 0) { throw "the Setup exe could not install into a test folder: $(Get-Content $ilog -Raw)" }
$bad = @($installFiles | Where-Object {
    $rel = $_.FullName.Substring($payloadDir.Length + 1)
    (Get-FileHash $_.FullName).Hash -ne (Get-FileHash "$fake\$rel" -ErrorAction SilentlyContinue).Hash })
if ($bad.Count) { throw "the Setup exe installed files that differ from the package: $($bad.Name -join ', ')" }
if ((Get-FileHash "$fake\Lossless_original.dll").Hash -ne (Get-FileHash $fakeOriginal).Hash) { throw 'the Setup exe did not keep the original Lossless.dll' }
Write-Host "  Setup exe checked: reports $Version and installs $($installFiles.Count) files byte for byte into a test folder"
# --extract, for installing by hand: the same files, byte for byte, into a new folder
$ex = "$check\extract"
$elog = "$check\extract.log"
$p = Start-Process $setupExe -ArgumentList '--extract', "`"$ex`"", '--no-remember', '--log', "`"$elog`"" -PassThru -Wait -WindowStyle Hidden
if ($p.ExitCode -ne 0) { throw "the Setup exe could not extract its files: $(Get-Content $elog -Raw)" }
$want = @(Get-ChildItem $payloadDir -Recurse -File)
$bad = @($want | Where-Object { (Get-FileHash $_.FullName).Hash -ne (Get-FileHash ("$ex\" + $_.FullName.Substring($payloadDir.Length + 1)) -ErrorAction SilentlyContinue).Hash })
if ($bad.Count -or @(Get-ChildItem $ex -Recurse -File).Count -ne $want.Count) { throw "the Setup exe's --extract gave other files than the package: $($bad.Name -join ', ')" }
Write-Host "  Setup exe checked: --extract writes the $($want.Count) files byte for byte"
RemoveUnder $check $env:TEMP
New-Item -ItemType Directory -Force $dist | Out-Null
$installer = "$dist\$name.exe"
Copy-Item $setupExe $installer
$setupHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
"$setupHash  $name.exe" | Set-Content -LiteralPath "$installer.sha256" -Encoding ascii
RemoveUnder $payloadDir $work
Write-Host "installer: $installer"
Write-Host "checksum: $installer.sha256"
Write-Host "  addons included: $($included -join ', ')"
