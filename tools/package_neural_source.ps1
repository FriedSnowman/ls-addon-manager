# Corresponding source for the GPL NR addon, including every statically linked ImGui source.
param([string]$Destination, [string]$ImGuiSource, [string]$NeuralBuildDir = 'addons\NeuralRendering\build')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
if(-not $Destination){$Destination=Join-Path $root 'build\NeuralRendering-source.zip'}
if(-not [IO.Path]::IsPathRooted($NeuralBuildDir)){$NeuralBuildDir=Join-Path $root $NeuralBuildDir}
if(-not $ImGuiSource){
    $cache=Join-Path $NeuralBuildDir 'CMakeCache.txt'
    if(Test-Path -LiteralPath $cache){$entry=Select-String -LiteralPath $cache -Pattern '^FETCHCONTENT_SOURCE_DIR_IMGUI:PATH=(.+)$';if($entry){$ImGuiSource=$entry.Matches[0].Groups[1].Value}}
    if(-not $ImGuiSource){$ImGuiSource=Join-Path $NeuralBuildDir '_deps\imgui-src'}
}
if(-not(Test-Path -LiteralPath (Join-Path $ImGuiSource 'imgui.cpp'))){throw 'Build the NR addon first, or specify -ImGuiSource for its pinned ImGui sources.'}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$parent=Split-Path ([IO.Path]::GetFullPath($Destination)) -Parent
New-Item -ItemType Directory -Force $parent | Out-Null
$stream=[IO.File]::Create($Destination)
$archive=New-Object IO.Compression.ZipArchive($stream,[IO.Compression.ZipArchiveMode]::Create)
try {
    $sources=@(git -C $root ls-files --cached -- addons/NeuralRendering manager/sdk/include)
    foreach($relative in $sources){
        if($relative -match '\.(dll|exe|lib|pdb|zip)$'){throw "Binary is not corresponding source: $relative"}
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,(Join-Path $root $relative),$relative.Replace('\','/'),[IO.Compression.CompressionLevel]::Optimal)
    }
    foreach($file in 'imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','imgui.h','imgui_internal.h','imconfig.h','imstb_rectpack.h','imstb_textedit.h','imstb_truetype.h','LICENSE.txt'){
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,(Join-Path $ImGuiSource $file),('dependencies/imgui/'+$file),[IO.Compression.CompressionLevel]::Optimal)
    }
    foreach($relative in 'tools/package_neural_source.ps1','LICENSE'){
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,(Join-Path $root $relative),$relative,[IO.Compression.CompressionLevel]::Optimal)
    }
    $entry=$archive.CreateEntry('BUILD.txt');$writer=New-Object IO.StreamWriter($entry.Open())
    $writer.WriteLine("Corresponding source from workspace revision $(git -C $root rev-parse HEAD), including current modifications.")
    $writer.WriteLine('Build with Visual Studio 2022 C++ tools, Windows SDK and CMake:')
    $writer.WriteLine('cmake -S addons/NeuralRendering -B build -G "Visual Studio 17 2022" -A x64')
    $writer.WriteLine('cmake --build build --config Release --target DLSSNR02')
    $writer.WriteLine('The included ImGui sources are the pinned 367b2c24f399988ddafc0bb4628da0106bcc09be build dependency. See addon NOTICE.md and LICENSE.')
    $writer.Dispose()
} finally {$archive.Dispose();$stream.Dispose()}
Write-Host "NR corresponding source: $Destination"
