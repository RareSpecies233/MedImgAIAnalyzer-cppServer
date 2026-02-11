<#
Windows build helper for MedImgAIAnalyzer-cppServer
Features:
 - optionally bootstrap/use vcpkg and install dependencies (OpenCV, onnxruntime)
 - configure with CMake (Ninja or Visual Studio)
 - build and verify `MedImgAIAnalyzer.exe`

Usage:
  .\build-windows.ps1 [-UseVcpkg] [-Triplet x64-windows] [-Config Release]
#>

[CmdletBinding()]
param(
    [switch]$UseVcpkg = $true,
    [string]$Triplet = "x64-windows",
    [ValidateSet('Release','Debug')][string]$Config = "Release",
    [switch]$AutoInstallCMake = $false
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Check-Tool([string]$name, [string]$exe) {
    $p = Get-Command $exe -ErrorAction SilentlyContinue
    if (-not $p) { Write-Host "⚠️ $name not found in PATH" -ForegroundColor Yellow; return $false }
    Write-Host "✅ Found ${name}: $($p.Source)" -ForegroundColor Green; return $true
}

Write-Host "== MedImgAIAnalyzer — Windows build helper ==" -ForegroundColor Cyan
Push-Location $PSScriptRoot

# Ensure CMake is available (try PATH, common locations, or optionally install)
$cmakeFound = Check-Tool "CMake" "cmake"
if (-not $cmakeFound) {
    # Look in common install locations used on Windows
    $possiblePaths = @(
        "$Env:ProgramFiles\\CMake\\bin\\cmake.exe",
        "$Env:ProgramFiles(x86)\\CMake\\bin\\cmake.exe",
        "$Env:ProgramFiles\\Microsoft Visual Studio\\2022\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe"
    )
    foreach ($p in $possiblePaths) {
        if (Test-Path $p) {
            Write-Host "✅ Found CMake at $p" -ForegroundColor Green
            $cmakeFound = $true
            $env:PATH = "$([System.IO.Path]::GetDirectoryName($p));$env:PATH"
            break
        }
    }
}

if (-not $cmakeFound -and $AutoInstallCMake) {
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        Write-Host "Attempting to install CMake via winget..." -ForegroundColor Cyan
        winget install --id Kitware.CMake -e --accept-package-agreements --accept-source-agreements
    } elseif (Get-Command choco -ErrorAction SilentlyContinue) {
        Write-Host "Attempting to install CMake via chocolatey..." -ForegroundColor Cyan
        choco install cmake --installargs '"ADD_CMAKE_TO_PATH=System"' -y
    } else {
        Write-Host "No supported package manager (winget/choco) found to auto-install CMake." -ForegroundColor Yellow
    }
    $cmakeFound = Check-Tool "CMake" "cmake"
}

if (-not $cmakeFound) {
    Write-Error "CMake is required in PATH. Install it (recommended):`n  - winget: `n      winget install --id Kitware.CMake -e`n  - choco: `n      choco install cmake --installargs '"ADD_CMAKE_TO_PATH=System"' -y`nOr open 'x64 Native Tools Command Prompt for VS 2022' and re-run this script."
    throw 'CMake is required in PATH.'
}

$hasNinja = Check-Tool "Ninja" "ninja"
$generator = if ($hasNinja) { 'Ninja' } else { 'Visual Studio 17 2022' }
Write-Host "Using CMake generator: $generator"

$vcpkgRoot = $env:VCPKG_ROOT
if ($UseVcpkg -and -not $vcpkgRoot) {
    $possible = Join-Path $PSScriptRoot 'vcpkg'
    if (Test-Path $possible) { $vcpkgRoot = $possible }
}

if ($UseVcpkg) {
    if (-not $vcpkgRoot) {
        Write-Host "Cloning vcpkg to .\\vcpkg (this requires network)" -ForegroundColor Yellow
        git clone https://github.com/microsoft/vcpkg.git vcpkg
        Push-Location vcpkg
        .\bootstrap-vcpkg.bat
        Pop-Location
        $vcpkgRoot = Join-Path $PSScriptRoot 'vcpkg'
    }
    $vcpkgExe = Join-Path $vcpkgRoot 'vcpkg.exe'
    if (-not (Test-Path $vcpkgExe)) { throw 'vcpkg not bootstrapped correctly.' }
    Write-Host "Using vcpkg at: $vcpkgRoot"

    # Ensure required packages
    $pkgs = @('opencv', 'onnxruntime', 'asio')
    foreach ($p in $pkgs) {
        Write-Host "Installing vcpkg package: ${p}:$Triplet (may be a no-op if already installed)"
        & $vcpkgExe install "${p}:$Triplet"
    }
}

$buildDir = Join-Path $PSScriptRoot 'build'
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

$toolchainArg = ''
if ($vcpkgRoot) {
    $toolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    if (Test-Path $toolchain) { $toolchainArg = "-DCMAKE_TOOLCHAIN_FILE=$toolchain" }
}

$cmakeArgs = @(
    '-S', $PSScriptRoot,
    '-B', $buildDir,
    '-G', $generator,
    '-DCMAKE_BUILD_TYPE=' + $Config
)
if ($toolchainArg) { $cmakeArgs += $toolchainArg }

Write-Host "Configuring (this may download Crow header)" -ForegroundColor Cyan
& cmake @cmakeArgs

Write-Host "Building (config: $Config)" -ForegroundColor Cyan
& cmake --build $buildDir --config $Config --parallel

$exePath = Join-Path $buildDir 'bin\MedImgAIAnalyzer.exe'
if (-not (Test-Path $exePath)) {
    # Try Release-configured location for multi-config generators
    $exePath = Join-Path $buildDir "${Config}\MedImgAIAnalyzer.exe"
}

if (-not (Test-Path $exePath)) {
    Write-Error "build succeeded but main.exe not found. Look in: $buildDir"; exit 2
}

Write-Host "✅ Built: $exePath" -ForegroundColor Green

# If vcpkg provided DLLs, copy required DLLs next to exe so runtime runs
if ($vcpkgRoot) {
    $installed = Join-Path $vcpkgRoot 'installed\' + $Triplet + '\bin'
    if (Test-Path $installed) {
        Write-Host "Copying vcpkg runtime DLLs to output folder" -ForegroundColor Cyan
        Get-ChildItem -Path $installed -Filter '*.dll' -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -Path $_.FullName -Destination (Split-Path $exePath) -Force
        }
    }
}

Write-Host "Running quick smoke test: MedImgAIAnalyzer.exe --help" -ForegroundColor Cyan
$proc = Start-Process -FilePath $exePath -ArgumentList '--help' -NoNewWindow -PassThru -Wait -ErrorAction SilentlyContinue
if ($proc.ExitCode -in 0,1) {
    Write-Host "✅ Smoke test passed (exit code $($proc.ExitCode))." -ForegroundColor Green
    Write-Host "You can run: $exePath --onnx <model.onnx>" -ForegroundColor Green
    exit 0
} else {
    Write-Warning "Smoke test returned exit code $($proc.ExitCode). The executable exists but may require additional runtime DLLs or files."; exit $proc.ExitCode
}
