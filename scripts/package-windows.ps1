# Build OurNotes for Windows and package it:
#   dist\OurNotes-<version>-windows-x64.zip
# holding OurNotes\bin\ournotes.exe with the Qt, OpenSSL and Visual C++ runtime
# DLLs it needs, and ournotes-cli.exe and ournotesd.exe next to it.
#
# Usage: scripts\package-windows.ps1 [-QtDir DIR] [-BuildDir DIR] [-OutDir DIR] [-NoBuild]
#   -QtDir     Qt 6 for MSVC (default: $env:QT_ROOT_DIR), e.g. C:\Qt\6.8.3\msvc2022_64
#   -BuildDir  build directory (default: build-windows\)
#   -OutDir    where the zip is written (default: dist\)
#   -NoBuild   package what was built before
#
# Needs Visual Studio 2022, CMake, and vcpkg (VCPKG_ROOT) with OpenSSL (MuPDF is
# downloaded and built with MSBuild):
#   vcpkg install openssl --triplet x64-windows
param(
    [string]$QtDir = $env:QT_ROOT_DIR,
    [string]$BuildDir = "",
    [string]$OutDir = "",
    [switch]$NoBuild
)
$ErrorActionPreference = "Stop"

$SrcDir = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $SrcDir "build-windows" }
if (-not $OutDir) { $OutDir = Join-Path $SrcDir "dist" }

function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed (exit code $LASTEXITCODE)" }
}

# --- build ---------------------------------------------------------------------------------
if (-not $NoBuild) {
    if (-not $QtDir -or -not (Test-Path (Join-Path $QtDir "lib\cmake\Qt6"))) {
        throw "no Qt 6 found (pass -QtDir, e.g. C:\Qt\6.8.3\msvc2022_64)"
    }
    $vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { $env:VCPKG_INSTALLATION_ROOT }
    if (-not $vcpkg) { throw "set VCPKG_ROOT to your vcpkg directory" }
    Write-Host "==> Configuring (Qt from $QtDir)"
    Invoke-Native cmake @("-S", $SrcDir, "-B", $BuildDir,
        "-DCMAKE_TOOLCHAIN_FILE=$vcpkg\scripts\buildsystems\vcpkg.cmake",
        "-DCMAKE_PREFIX_PATH=$QtDir",
        "-DOURNOTES_BUILD_TESTS=OFF",
        "-DOURNOTES_DEPLOY_QT=ON",           # windeployqt at install time
        "-DX_VCPKG_APPLOCAL_DEPS_INSTALL=ON") # and vcpkg's DLLs
    Write-Host "==> Building"
    Invoke-Native cmake @("--build", $BuildDir, "--config", "Release", "--parallel")
}
$versionHeader = Join-Path $BuildDir "generated\ournotes\version.h"
if (-not (Test-Path $versionHeader)) { throw "nothing built in $BuildDir (run without -NoBuild)" }
$version = (Select-String -Path $versionHeader -Pattern 'OURNOTES_VERSION "([^"]+)"').Matches[0].Groups[1].Value

# --- package ------------------------------------------------------------------------------
$stage = Join-Path $BuildDir "package"
$root = Join-Path $stage "OurNotes"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
Write-Host "==> Installing into $root"
Invoke-Native cmake @("--install", $BuildDir, "--config", "Release", "--prefix", $root)
foreach ($file in "bin\ournotes.exe", "bin\ournotes-cli.exe", "bin\ournotesd.exe", "share\ournotes\microtex") {
    if (-not (Test-Path (Join-Path $root $file))) { throw "$file is missing from the package" }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "OurNotes-$version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Write-Host "==> Creating $(Split-Path -Leaf $zip)"
Compress-Archive -Path $root -DestinationPath $zip
Write-Host "==> Done: $zip"
