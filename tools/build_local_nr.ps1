# Builds this checkout and its pinned source dependencies, then stages only the NR addon.
# Does not run the resulting programs, install into Lossless Scaling, or fetch a model.
param([string]$CMake = "cmake", [string]$Git = "git")
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'local-build-lock.json') -Raw | ConvertFrom-Json

function HashFile([string]$path, [bool]$normalize = $false) {
    if (-not $normalize) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    $bytes = [Text.Encoding]::UTF8.GetBytes(([IO.File]::ReadAllText($path) -replace "`r`n", "`n"))
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hasher.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant() }
    finally { $hasher.Dispose() }
}
# Resolve local tools for execution; provenance records their hashes without installation paths.
$cmakeExe = (Get-Command $CMake -CommandType Application -ErrorAction Stop).Source
$gitExe = (Get-Command $Git -CommandType Application -ErrorAction Stop).Source
$toolchain = @{
    cmake = @{ sha256 = HashFile $cmakeExe }
    git = @{ sha256 = HashFile $gitExe }
    generator = 'Visual Studio 17 2022'; architecture = 'x64'
}
function RunCMake([string[]]$arguments) {
    & $cmakeExe @arguments
    if ($LASTEXITCODE -ne 0) { throw "CMake failed ($LASTEXITCODE); no package will be produced." }
}

# A new snapshot, build directory and package on every invocation. No cache or binary reuse.
$buildRoot = Join-Path $repoRoot ('build\local-nr-' + [Guid]::NewGuid().ToString('N'))
$sourceRoot = Join-Path $buildRoot 'source'
$stageRoot = Join-Path $buildRoot 'package'
New-Item -ItemType Directory -Path $sourceRoot | Out-Null
$files = @(& $gitExe -C $repoRoot -c core.quotePath=false ls-files --cached --others --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate the reviewed checkout.' }
$inputs = @()
foreach ($relative in ($files | Sort-Object -Unique)) {
    $inputPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $relative))
    if (-not $inputPath.StartsWith($repoRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Source path escapes the repository.' }
    if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) { continue } # removed tracked source
    if ([IO.Path]::GetExtension($inputPath) -in '.dll', '.exe', '.lib', '.obj', '.zip') { throw "Prebuilt file in source list: $relative" }
    $current = Get-Item -LiteralPath $inputPath
    while ($current.FullName -ne $repoRoot) {
        if ($current.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked source is not accepted: $relative" }
        $current = Get-Item -LiteralPath (Split-Path $current.FullName -Parent)
    }
    $destination = Join-Path $sourceRoot $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath $inputPath -Destination $destination
    $inputs += @{ path = $relative; sha256 = HashFile $destination }
}

# The only prebuilt project dependency: NVIDIA's explicitly identified SDK static library.
# Headers and licence are verified too. The public fetch script is optional provisioning, not invoked here.
foreach ($file in $lock.ngx.files) {
    $relative = 'addons/DLSS5NR01/external/ngx/' + $file.local
    $inputPath = Join-Path $repoRoot $relative
    $normalize = $file.local.EndsWith('.h')
    if ((HashFile $inputPath $normalize) -ne $file.sha256) { throw "NVIDIA SDK mismatch: $relative" }
    $destination = Join-Path $sourceRoot $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath $inputPath -Destination $destination
    if ((HashFile $destination $normalize) -ne $file.sha256) { throw "SDK copy changed: $relative" }
    $inputs += @{ path = $relative; sha256 = HashFile $destination; vendor = 'NVIDIA' }
}
foreach ($file in $lock.vendored) {
    if ((HashFile (Join-Path $sourceRoot $file.path) $true) -ne $file.sha256) { throw "Vendored source mismatch: $($file.path)" }
}

$deps = Join-Path $buildRoot 'dependencies'
$managerBuild = Join-Path $buildRoot 'manager'
$addonBuild = Join-Path $buildRoot 'nr'
$options = @('-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DGIT_EXECUTABLE=$gitExe", "-DFETCHCONTENT_BASE_DIR=$deps", '-DNR_FRAMEGEN=OFF')
RunCMake -arguments (@('-S', "$sourceRoot\manager", '-B', $managerBuild) + $options)
RunCMake -arguments (@('-S', "$sourceRoot\addons\DLSS5NR01", '-B', $addonBuild) + $options)
foreach ($dependency in $lock.sources) {
    $source = Join-Path $deps ($dependency.name + '-src')
    $revision = & $gitExe -C $source rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $revision -ne $dependency.commit) { throw "Source revision mismatch: $($dependency.name)" }
    $remote = & $gitExe -C $source remote get-url origin
    if ($LASTEXITCODE -ne 0 -or $remote -ne $dependency.repository) { throw "Source origin mismatch: $($dependency.name)" }
}
RunCMake -arguments @('--build', $managerBuild, '--config', 'Release', '--target', 'Lossless')
RunCMake -arguments @('--build', $addonBuild, '--config', 'Release', '--target', 'DLSS5NR01', 'nr_forwarder', 'nr_selftest')

$addonStage = Join-Path $stageRoot 'addons\DLSS5NR01'
New-Item -ItemType Directory -Path $addonStage | Out-Null
Copy-Item -LiteralPath "$managerBuild\Release\Lossless.dll" -Destination $stageRoot
foreach ($name in 'DLSS5NR01.dll', 'nvngx.dll_dlss5nr01.dll', 'nr_selftest.exe') {
    Copy-Item -LiteralPath "$addonBuild\Release\$name" -Destination $addonStage
}
foreach ($name in 'manager-icon.ico', 'manager-icon.png') { Copy-Item -LiteralPath "$sourceRoot\manager\$name" -Destination $stageRoot }
foreach ($name in 'addon.json', 'icon.svg', 'LICENSE') { Copy-Item -LiteralPath "$sourceRoot\addons\DLSS5NR01\$name" -Destination $addonStage }
Copy-Item -LiteralPath "$sourceRoot\addons\DLSS5NR01\external\ngx\LICENSE.txt" -Destination "$addonStage\NVIDIA-LICENSE.txt"
foreach ($name in 'LICENSE', 'NOTICE.md', 'DISCLAIMER.md') { Copy-Item -LiteralPath "$sourceRoot\$name" -Destination $stageRoot }
$outputs = @(Get-ChildItem -LiteralPath $stageRoot -File -Recurse | ForEach-Object {
    @{ path = $_.FullName.Substring($stageRoot.Length + 1); sha256 = HashFile $_.FullName }
})
@{ generated_utc = [DateTime]::UtcNow.ToString('o'); source_inputs = $inputs; dependencies = $lock;
   toolchain = $toolchain; outputs = $outputs; runtime_tests_run = $false; model_included = $false } |
    ConvertTo-Json -Depth 12 | Set-Content -LiteralPath "$stageRoot\build-provenance.json" -Encoding UTF8
Write-Host "Locally compiled NR-only package: $stageRoot"
Write-Host 'Package not installed; no built program was run. The proprietary neural model is not included.'
