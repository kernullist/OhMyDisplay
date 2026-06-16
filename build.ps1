# OhMyDisplay - configure + build using the VS 2022 BuildTools toolchain (Ninja).
# Usage: pwsh -File build.ps1 [-Config Debug|Release] [-Target <name>] [-Clean]
param(
    [string]$Config = "Debug",
    [string]$Target = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }
if (-not (Test-Path $cmake))  { throw "cmake not found: $cmake" }

# Import the MSVC x64 environment (INCLUDE/LIB/PATH) into this session.
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2]
    }
}

$root = $PSScriptRoot
$buildDir = Join-Path $root "build"

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

# Configure if the build tree is not yet generated.
if (-not (Test-Path (Join-Path $buildDir "build.ninja"))) {
    & $cmake -S $root -B $buildDir -G "Ninja" `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_BUILD_TYPE=$Config" `
        "-DCMAKE_C_COMPILER=cl" `
        "-DCMAKE_CXX_COMPILER=cl"
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}

$args = @("--build", $buildDir)
if ($Target -ne "") { $args += @("--target", $Target) }

& $cmake @args
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "BUILD OK ($Config)" -ForegroundColor Green
