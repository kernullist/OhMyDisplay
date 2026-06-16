# OhMyDisplay - configure + build with the MSVC toolchain (Ninja).
# Works for any VS 2022 edition (BuildTools/Community/Pro/Enterprise) and on GitHub
# Actions runners by locating the install via vswhere instead of a fixed path.
# Usage: pwsh -File build.ps1 [-Config Debug|Release] [-Target <name>] [-Clean]
param(
    [string]$Config = "Debug",
    [string]$Target = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

# Locate any VS install that has the x64 C++ toolset.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found: $vswhere" }
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($vsPath)) { throw "No VS install with the x64 C++ toolset was found" }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

# Prefer the CMake/Ninja bundled with VS; fall back to whatever is on PATH (CI runners).
$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not (Test-Path $ninja)) { $ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source }
if ([string]::IsNullOrWhiteSpace($cmake)) { throw "cmake not found (not bundled with VS and not on PATH)" }

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
    $cmakeArgs = @(
        "-S", $root, "-B", $buildDir, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$Config",
        "-DCMAKE_C_COMPILER=cl",
        "-DCMAKE_CXX_COMPILER=cl"
    )
    # Pin the make program only when we actually found a Ninja binary.
    if (-not [string]::IsNullOrWhiteSpace($ninja)) {
        $cmakeArgs += "-DCMAKE_MAKE_PROGRAM=$ninja"
    }
    & $cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}

$args = @("--build", $buildDir)
if ($Target -ne "") { $args += @("--target", $Target) }

& $cmake @args
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "BUILD OK ($Config)" -ForegroundColor Green
