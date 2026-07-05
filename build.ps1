param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $Root "build"

$CMakeExe = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
if (-not $CMakeExe) {
    $BundledCMake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path -LiteralPath $BundledCMake) {
        $CMakeExe = $BundledCMake
    }
}

if (-not $CMakeExe -or -not (Test-Path -LiteralPath $CMakeExe)) {
    throw "cmake.exe not found. Install CMake or Visual Studio 2022 Build Tools with CMake support."
}

if (-not (Test-Path -LiteralPath $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

& $CMakeExe -S $Root -B $BuildDir -G "Visual Studio 17 2022" -A x64
& $CMakeExe --build $BuildDir --config $Configuration
