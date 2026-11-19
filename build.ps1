param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$RepoRoot = $PSScriptRoot
$SourceDir = Join-Path $RepoRoot "UniversalHookX"
$BuildDir = Join-Path $SourceDir "build"

function Find-ToolDirectory {
    param(
        [string]$ExecutableName,
        [string[]]$CandidateDirectories
    )

    $existing = Get-Command $ExecutableName -ErrorAction SilentlyContinue
    if ($existing) {
        return Split-Path -Parent $existing.Source
    }

    foreach ($dir in $CandidateDirectories) {
        if ($dir -and (Test-Path (Join-Path $dir $ExecutableName))) {
            return $dir
        }
    }

    return $null
}

$winGetMingwBins = @()
$winGetPackages = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
if (Test-Path $winGetPackages) {
    $winGetMingwBins = @(Get-ChildItem -Path $winGetPackages -Directory -Filter "BrechtSanders.WinLibs*" -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "mingw64\bin" })
}

$winGetLinks = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links"

$mingwDir = Find-ToolDirectory -ExecutableName "g++.exe" -CandidateDirectories ($winGetMingwBins + @(
    $winGetLinks,
    "C:\mingw64\bin",
    "C:\msys64\mingw64\bin",
    "C:\msys64\ucrt64\bin"
))

if (-not $mingwDir) {
    throw "MinGW-w64 g++.exe not found. Install WinLibs (BrechtSanders.WinLibs.POSIX.UCRT) or add MinGW-w64 to PATH."
}

$vsCmakeDir = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$vsNinjaDir = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

$cmakeDir = Find-ToolDirectory -ExecutableName "cmake.exe" -CandidateDirectories @($mingwDir, $vsCmakeDir)
if (-not $cmakeDir) {
    throw "cmake.exe not found."
}

$ninjaDir = Find-ToolDirectory -ExecutableName "ninja.exe" -CandidateDirectories @($mingwDir, $vsNinjaDir)

$pathEntries = @($mingwDir, $cmakeDir)
if ($ninjaDir) {
    $pathEntries += $ninjaDir
}
$env:PATH = ($pathEntries -join ";") + ";" + $env:PATH

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

$generatorArgs = @()
if ($ninjaDir) {
    $generatorArgs = @("-G", "Ninja")
} elseif (Test-Path (Join-Path $mingwDir "mingw32-make.exe")) {
    $generatorArgs = @("-G", "MinGW Makefiles")
} else {
    throw "Neither ninja.exe nor mingw32-make.exe was found."
}

& cmake -S $SourceDir -B $BuildDir @generatorArgs `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_C_COMPILER=gcc" `
    "-DCMAKE_CXX_COMPILER=g++" `
    "-DUHX_ENABLE_VULKAN=OFF"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& cmake --build $BuildDir -j
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$outputDll = Join-Path $SourceDir "bin\KCD2-Internal $Config x64.dll"
if (Test-Path $outputDll) {
    Write-Host "Build succeeded: $outputDll"
} else {
    Write-Warning "Build finished, but expected output DLL was not found at $outputDll"
}
