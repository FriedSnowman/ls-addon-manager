# Builds Release and assembles dist\LSAddonManager-<version>-x64.zip: LSAddonManagerSetup.exe, one file that carries the manager and the addons
# that build, with an install note and the licences. The files are not in the zip a second time: `LSAddonManagerSetup.exe --extract <folder>`
# writes them out for installing by hand (since 0.9.12; the zip held every file twice before).
# The user-provided NR model is never packaged. All four built addon products are required.
# GPL NR corresponding source and its exact ImGui build dependency ship inside the payload.
#   powershell -File tools/package.ps1 [-Version 0.9.15]
param(
    [string]$Version = '',
    [switch]$IncludeWip   # also package addons marked work in progress (none are now)
)
$ErrorActionPreference = 'Stop'
function RemoveUnder([string]$Path,[string]$Parent) {
    $absolute=[IO.Path]::GetFullPath($Path)
    $boundary=[IO.Path]::GetFullPath($Parent).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw "Refusing removal outside $boundary : $absolute"}
    if(Test-Path -LiteralPath $absolute){Remove-Item -LiteralPath $absolute -Recurse -Force}
}
$root = Split-Path $PSScriptRoot -Parent
if (-not $Version) {   # default: the release number in version.h
    $Version = (Select-String -Path "$root\manager\sdk\include\eam\version.h" -Pattern 'EAM_VERSION_STRING "([^"]+)"').Matches[0].Groups[1].Value
}
# Always build locally; downloaded or stale release artifacts are not a fallback.
& powershell -NoProfile -File "$PSScriptRoot\build_all.ps1" -Only host,neural,scalers
if ($LASTEXITCODE -ne 0) { throw 'Local build failed; refusing to package existing artifacts.' }

if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Version must be three nonnegative numbers separated by dots.' }
$name = "LSAddonManager-$Version-x64"
$dist = "$root\dist"
$stage = "$dist\$name"
RemoveUnder $stage $dist
New-Item -ItemType Directory -Force "$stage\addons" | Out-Null

function Need($path, $what) { if (-not (Test-Path $path)) { throw "missing build output: $what ($path)" } }
Need "$root\manager\build\Release\Lossless.dll" 'the manager'
Copy-Item "$root\manager\build\Release\Lossless.dll" $stage
Copy-Item "$root\manager\manager-icon.ico", "$root\manager\manager-icon.png" $stage

& "$PSScriptRoot\package_neural_source.ps1" -Destination "$root\build\NeuralRendering-source.zip"

$addons = @(
    @{ Id = 'DLSSNR02'; Dir = "$root\addons\NeuralRendering"; Bin = "$root\addons\NeuralRendering\build\Release";
       Files = @('DLSSNR02.dll','NeuralRendering_nvngx.dll');
       Extra = @{ 'NOTICE.md' = "$root\addons\NeuralRendering\NOTICE.md"; 'README.md' = "$root\addons\NeuralRendering\README.md";
                  'NeuralRendering-source.zip' = "$root\build\NeuralRendering-source.zip" } },
    # DLSS Upscaler (a preview since 0.9.1: its addon.json keeps it off until switched on): built from the same sources; its addon.json lives
    # in products\DLSS4DLAA, and NVIDIA's DLSS runtime ships in its dlss folder
    @{ Id = 'DLSS4DLAA'; Dir = "$root\addons\Upscalers\products\DLSS4DLAA"; Bin = "$root\addons\Upscalers\build\Release"; Files = @('DLSS4DLAA.dll');
       Extra = @{ 'NVIDIA-LICENSE.txt' = "$root\addons\Upscalers\external\ngx\LICENSE.txt"; 'LICENSE.txt' = "$root\addons\Upscalers\LICENSE";
                  'dlss\nvngx_dlss.dll' = "$root\addons\Upscalers\external\ngx\bin\nvngx_dlss.dll" } },
    # FSR Upscaler: the same sources again; AMD's FidelityFX runtime (MIT, signed by AMD) ships in its fsr folder (tools\fetch_ffx_sdk.ps1)
    @{ Id = 'FSR3UPSC'; Dir = "$root\addons\Upscalers\products\FSR3UPSC"; Bin = "$root\addons\Upscalers\build\Release"; Files = @('FSR3UPSC.dll');
       Extra = @{ 'AMD-FidelityFX-LICENSE.txt' = "$root\addons\Upscalers\third_party\ffx\LICENSE.txt"; 'LICENSE.txt' = "$root\addons\Upscalers\LICENSE";
                  'fsr\amd_fidelityfx_dx12.dll' = "$root\addons\Upscalers\external\ffx\bin\amd_fidelityfx_dx12.dll";
                  # the second choice in the manager's Runtimes list: FSR 4.1.1b, OptiScaler's build (tools\fetch_fsr4.ps1)
                  'runtimes\FSR\0dd77d9c\amd_fidelityfx_dx12.dll' = "$root\addons\Upscalers\external\fsr4\amd_fidelityfx_dx12.dll";
                  'runtimes\FSR\0dd77d9c\ABOUT.txt' = "$root\addons\Upscalers\products\FSR3UPSC\FSR4-ABOUT.txt";
                  'runtimes\FSR\0dd77d9c\LICENSE.txt' = "$root\addons\Upscalers\third_party\ffx4\LICENSE.txt" } },
    # XeSS Upscaler: the same sources again; Intel's XeSS runtime (Intel Simplified Software License, signed by Intel) ships in its xess folder,
    # with Intel's licence next to it, as that licence asks (tools\fetch_xess_sdk.ps1)
    @{ Id = 'XESSUPSC'; Dir = "$root\addons\Upscalers\products\XESSUPSC"; Bin = "$root\addons\Upscalers\build\Release"; Files = @('XESSUPSC.dll');
       Extra = @{ 'Intel-XeSS-LICENSE.txt' = "$root\addons\Upscalers\external\xess\LICENSE.txt"; 'LICENSE.txt' = "$root\addons\Upscalers\LICENSE";
                  'xess\libxess.dll' = "$root\addons\Upscalers\external\xess\bin\libxess.dll" } }
)
$included = @(); $skipped = @()
foreach ($a in $addons) {
    if ($a.Wip -and -not $IncludeWip) { Write-Host "$($a.Id) is work in progress: not packaged (-IncludeWip packages it)."; continue }
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

# The single-file installer: the files above (only the ones that get installed) are packed into a bundle that becomes a resource of Setup.exe.
# It is built in its own folder (installer\build_setup), so the test build of the installer, which carries no files, stays as the tests expect.
$setupBuild = "$root\installer\build_setup"
$payloadDir = "$dist\payload-$Version"
RemoveUnder $payloadDir $dist
New-Item -ItemType Directory -Force $payloadDir | Out-Null
Copy-Item "$stage\Lossless.dll", "$stage\manager-icon.ico", "$stage\manager-icon.png" $payloadDir
Copy-Item "$stage\addons" "$payloadDir\addons" -Recurse
& cmake -S "$root\installer" -B $setupBuild -G 'Visual Studio 17 2022' -A x64 | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Installer configure failed' }
& cmake --build $setupBuild --config Release --target pack_payload | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'the installer packer did not build' }
$bundle = "$dist\payload-$Version.bin"
& "$setupBuild\Release\pack_payload.exe" $payloadDir $bundle
if ($LASTEXITCODE -ne 0) { throw 'packing the installer files failed' }
& cmake -S "$root\installer" -B $setupBuild "-DSETUP_PAYLOAD=$bundle" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Installer payload configure failed' }
& cmake --build $setupBuild --config Release --target LSAddonManagerSetup 2>&1 | Select-String -Pattern ' error |warning C' | ForEach-Object { Write-Host $_.Line }
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed; refusing stale Setup output.' }
$setupExe = "$setupBuild\Release\LSAddonManagerSetup.exe"
if (-not (Test-Path $setupExe)) { throw 'the Setup exe did not build' }

# Check the finished exe as a person would use it: it must report the version it carries, and install into a fake Lossless Scaling folder byte for byte
$check = "$env:TEMP\setup_check_$PID"
RemoveUnder $check $env:TEMP
New-Item -ItemType Directory $check | Out-Null
$vlog = "$check\version.log"
$p = Start-Process $setupExe -ArgumentList '--version', '--no-remember', '--log', "`"$vlog`"" -PassThru -Wait -WindowStyle Hidden
$said = if (Test-Path $vlog) { (Get-Content $vlog -Raw).Trim() } else { '' }
if ($p.ExitCode -ne 0 -or $said -ne "payload $Version") { throw "the Setup exe reports '$said' (exit $($p.ExitCode)), expected 'payload $Version'" }
$fakeOriginal = "$root\installer\build\Release\fake_original.dll"
if (Test-Path $fakeOriginal) {
    $fake = "$check\ls"
    New-Item -ItemType Directory $fake | Out-Null
    Copy-Item "$env:SystemRoot\System32\cmd.exe" "$fake\LosslessScaling.exe"
    Copy-Item $fakeOriginal "$fake\Lossless.dll"
    $ilog = "$check\install.log"
    $p = Start-Process $setupExe -ArgumentList '--silent', 'install', '--folder', "`"$fake`"", '--no-remember', '--log', "`"$ilog`"" -PassThru -Wait -WindowStyle Hidden
    if ($p.ExitCode -ne 0) { throw "the Setup exe could not install into a test folder: $(Get-Content $ilog -Raw)" }
    $bad = @(Get-ChildItem $payloadDir -Recurse -File | Where-Object {
        $rel = $_.FullName.Substring($payloadDir.Length + 1)
        (Get-FileHash $_.FullName).Hash -ne (Get-FileHash "$fake\$rel" -ErrorAction SilentlyContinue).Hash })
    if ($bad.Count) { throw "the Setup exe installed files that differ from the package: $($bad.Name -join ', ')" }
    if ((Get-FileHash "$fake\Lossless_original.dll").Hash -ne (Get-FileHash $fakeOriginal).Hash) { throw 'the Setup exe did not keep the original Lossless.dll' }
    Write-Host "  Setup exe checked: reports $Version and installs $((Get-ChildItem $payloadDir -Recurse -File).Count) files byte for byte into a test folder"
} else {
    Write-Host '  (Setup exe installed-files check skipped: build the installer tests first to get the stand-in Lossless.dll)'
}
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
Copy-Item $setupExe "$stage\LSAddonManagerSetup.exe"
RemoveUnder $payloadDir $dist
# the files are inside Setup: not in the zip a second time
foreach($file in 'Lossless.dll','manager-icon.ico','manager-icon.png','addons'){RemoveUnder (Join-Path $stage $file) $stage}

Copy-Item "$root\LICENSE" "$stage\LICENSE.txt"
Copy-Item "$root\NOTICE.md", "$root\DISCLAIMER.md", "$root\CHANGELOG.md" $stage
@"
LS Addon Manager $Version

Close Lossless Scaling before installing. Run LSAddonManagerSetup.exe to install,
repair or remove the manager. Existing files are backed up and failures roll back.
For manual installation, --extract files extracts the payload. Preserve the original
Lossless.dll as Lossless_original.dll before replacing the proxy.

Only locally built addons are included. The retired DLSS5NR01 plugin is ignored by
the manager. Its owned executables move to backups on upgrade; user models and
settings stay at their paths. DLSSNR02 uses fresh settings and NVIDIA hardware only.
No neural model is included. Corresponding NR source and licences are in the addon folder.
See README.md, NOTICE.md and DISCLAIMER.md in the source checkout.
"@ | Set-Content -Encoding UTF8 "$stage\INSTALL.txt"

$zip = "$dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
# Entry by entry, with "/" in the names: Compress-Archive and ZipFile.CreateFromDirectory in Windows PowerShell write backslashes as path
# separators, which the zip format does not allow and unzip tools on other systems complain about.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$fileStream = [IO.File]::Create($zip)
$archive = New-Object IO.Compression.ZipArchive($fileStream, [IO.Compression.ZipArchiveMode]::Create)
Get-ChildItem $stage -Recurse -File | ForEach-Object {
    $entryName = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entryName, [IO.Compression.CompressionLevel]::Optimal)
}
$archive.Dispose()
$fileStream.Dispose()
Write-Host "package: $zip"
Write-Host "  addons included: $($included -join ', ')"
if ($skipped.Count) { Write-Host "  addons NOT included (not built): $($skipped -join ', ')" }
Get-ChildItem $stage -Recurse -File | ForEach-Object { '  ' + $_.FullName.Substring($stage.Length + 1) }
