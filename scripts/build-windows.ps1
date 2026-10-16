<#
.SYNOPSIS
  Build the J-ALERT decoder plugin for SDR++ on Windows (x64, MSVC).

.DESCRIPTION
  1. Locates Visual Studio (or Build Tools) with the x64 C++ toolset via
     vswhere and imports its vcvars64 environment.
  2. Fetches the SDR++ source (headers) and the SDR++ Windows build, from
     which import libraries are generated (scripts/fetch-windows-deps.py).
  3. Builds zlib statically.
  4. Configures and builds the plugin; the DLL lands in dist\windows-x64\.

  Requires: git, Python >= 3.14, Visual Studio 2022 (or Build Tools) with the
  "Desktop development with C++" workload (MSVC + CMake + Ninja).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1
.EXAMPLE
  scripts\build-windows.ps1 -BuildType Debug -SkipFetch
#>
param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo")]
    [string]$BuildType = "Release",
    [switch]$SkipFetch,
    [string]$ZlibVersion = "1.3.1"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$External = Join-Path $Root "external"

function Invoke-Checked {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

# ── 1. MSVC environment ───────────────────────────────────────────────────────
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio 2022 or Build Tools." }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "No Visual Studio installation with the x64 C++ toolset was found." }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
    Write-Host "==> Importing MSVC environment from $vcvars"
    cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
        if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}

# ── 2. SDR++ source + Windows import definitions ─────────────────────────────
if (-not $SkipFetch) {
    $sdrppDir = Join-Path $External "SDRPlusPlus"
    $branch = if ($env:SDRPP_BRANCH) { $env:SDRPP_BRANCH } else { "master" }
    if (Test-Path (Join-Path $sdrppDir ".git")) {
        Write-Host "==> Updating SDR++ source ($branch)"
        Invoke-Checked git @("-C", $sdrppDir, "fetch", "--depth", "1", "origin", $branch)
        Invoke-Checked git @("-C", $sdrppDir, "reset", "--hard", "origin/$branch")
    } else {
        Write-Host "==> Cloning SDR++ source ($branch)"
        Invoke-Checked git @("clone", "--depth", "1", "--branch", $branch,
            "https://github.com/AlexandreRouma/SDRPlusPlus.git", $sdrppDir)
    }
    $python = if ($env:PYTHON) { $env:PYTHON } else { "python" }
    Invoke-Checked $python @((Join-Path $PSScriptRoot "fetch-windows-deps.py"))
}

# ── 3. zlib (static) ──────────────────────────────────────────────────────────
$zlibInstall = Join-Path $External "zlib-windows"
$zlibLib = Join-Path $zlibInstall "lib\zlibstatic.lib"
if (-not (Test-Path $zlibLib)) {
    $zlibSrcRoot = Join-Path $External "zlib-src"
    $zlibSrc = Join-Path $zlibSrcRoot "zlib-$ZlibVersion"
    if (-not (Test-Path $zlibSrc)) {
        New-Item -ItemType Directory -Force $zlibSrcRoot | Out-Null
        $tgz = Join-Path $zlibSrcRoot "zlib.tar.gz"
        Write-Host "==> Downloading zlib $ZlibVersion"
        Invoke-WebRequest -UseBasicParsing -OutFile $tgz `
            "https://github.com/madler/zlib/releases/download/v$ZlibVersion/zlib-$ZlibVersion.tar.gz"
        Invoke-Checked tar @("-xzf", $tgz, "-C", $zlibSrcRoot)
    }
    $zlibBuild = Join-Path $zlibSrcRoot "build"
    Invoke-Checked cmake @("-S", $zlibSrc, "-B", $zlibBuild, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=$zlibInstall")
    Invoke-Checked cmake @("--build", $zlibBuild, "--target", "install")
}

# ── 4. Plugin ─────────────────────────────────────────────────────────────────
$build = Join-Path $Root "build\windows-x64"
Invoke-Checked cmake @("-S", $Root, "-B", $build, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DZLIB_LIBRARY=$zlibLib",
    "-DZLIB_INCLUDE_DIR=$(Join-Path $zlibInstall 'include')")
Invoke-Checked cmake @("--build", $build)

$dist = Join-Path $Root "dist\windows-x64"
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item -Force (Join-Path $build "j_alert_decoder.dll") $dist
Write-Host "==> Built $(Join-Path $dist 'j_alert_decoder.dll')"
Write-Host "    Copy it into the 'modules' folder of SDR++ (next to sdrpp.exe)."
