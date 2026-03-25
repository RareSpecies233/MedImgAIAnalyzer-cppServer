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
    [switch]$AutoInstallCMake = $false,
    [switch]$OfflineDeps = $true,
    [switch]$AutoInstallRagTools = $true
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Check-Tool([string]$name, [string]$exe) {
    $p = Get-Command $exe -ErrorAction SilentlyContinue
    if (-not $p) { Write-Host "⚠️ $name not found in PATH" -ForegroundColor Yellow; return $false }
    Write-Host "✅ Found ${name}: $($p.Source)" -ForegroundColor Green; return $true
}

function Find-ToolPath([string]$exe, [string[]]$CandidatePaths = @()) {
    $command = Get-Command $exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    foreach ($candidate in $CandidatePaths) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $expanded = [Environment]::ExpandEnvironmentVariables($candidate)
        if ($expanded.IndexOf('*') -ge 0 -or $expanded.IndexOf('?') -ge 0) {
            $match = Get-ChildItem -Path $expanded -File -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending |
                Select-Object -First 1
            if ($match) { return $match.FullName }
            continue
        }
        if (Test-Path $expanded) {
            return (Resolve-Path $expanded).Path
        }
    }

    return $null
}

function Copy-ToolFiles([string]$SourcePath, [string]$DestinationDir, [switch]$CopySiblingFiles = $false) {
    if (-not $SourcePath) { return }
    New-Item -ItemType Directory -Path $DestinationDir -Force | Out-Null
    if ($CopySiblingFiles) {
        Get-ChildItem -Path (Split-Path $SourcePath) -File -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -Path $_.FullName -Destination (Join-Path $DestinationDir $_.Name) -Force
        }
        return
    }
    Copy-Item -Path $SourcePath -Destination (Join-Path $DestinationDir (Split-Path $SourcePath -Leaf)) -Force
}

function Install-PopplerIfNeeded() {
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        Write-Host "Attempting to install Poppler via winget for PDF parsing..." -ForegroundColor Cyan
        winget install --id oschwartz10612.Poppler -e --accept-package-agreements --accept-source-agreements
        return
    }
    if (Get-Command choco -ErrorAction SilentlyContinue) {
        Write-Host "Attempting to install Poppler via chocolatey for PDF parsing..." -ForegroundColor Cyan
        choco install poppler -y
        return
    }
    Write-Host "No supported package manager (winget/choco) found to auto-install Poppler." -ForegroundColor Yellow
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
    "-DCMAKE_BUILD_TYPE=$Config"
)
if ($toolchainArg) { $cmakeArgs += $toolchainArg }
if ($OfflineDeps) {
    # Avoid FetchContent git update step for already populated deps (e.g., crow-src)
    $cmakeArgs += '-DFETCHCONTENT_UPDATES_DISCONNECTED=ON'
}

Write-Host "Configuring (this may download Crow header)" -ForegroundColor Cyan
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configure failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "Building (config: $Config)" -ForegroundColor Cyan
& cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake build failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

$exePath = Join-Path $buildDir 'bin\MedImgAIAnalyzer.exe'
if (-not (Test-Path $exePath)) {
    # Try Release-configured location for multi-config generators
    $exePath = Join-Path $buildDir "${Config}\MedImgAIAnalyzer.exe"
}

if (-not (Test-Path $exePath)) {
    Write-Error "build completed but MedImgAIAnalyzer.exe not found. Look in: $buildDir"; exit 2
}

Write-Host "✅ Built: $exePath" -ForegroundColor Green

# If vcpkg provided DLLs, copy required DLLs next to exe so runtime runs
if ($vcpkgRoot) {
    $installed = Join-Path $vcpkgRoot ("installed\$Triplet\bin")
    if (Test-Path $installed) {
        Write-Host "Copying vcpkg runtime DLLs to output folder" -ForegroundColor Cyan
        Get-ChildItem -Path $installed -Filter '*.dll' -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -Path $_.FullName -Destination (Split-Path $exePath) -Force
        }
    }
}

$outputDir = Split-Path $exePath -Parent
$ragToolsDir = Join-Path $outputDir 'tools'
$ragToolSummary = [ordered]@{}

$curlPath = Find-ToolPath 'curl.exe'
if ($curlPath) {
    $curlDest = Join-Path $ragToolsDir 'curl'
    Copy-ToolFiles -SourcePath $curlPath -DestinationDir $curlDest
    $ragToolSummary['curl'] = "bundled ($curlPath)"
} else {
    $ragToolSummary['curl'] = 'missing'
    Write-Warning 'curl.exe not found. LLM chat requests will fail until curl is installed or placed under build/bin/tools/curl.'
}

$pdftotextCandidates = @(
    $env:MEDIMG_PDFTOTEXT,
    (Join-Path $PSScriptRoot 'tools\poppler\bin\pdftotext.exe'),
    (Join-Path $PSScriptRoot 'tools\poppler\Library\bin\pdftotext.exe'),
    "$Env:ProgramFiles\poppler\Library\bin\pdftotext.exe",
    "$Env:ProgramFiles\poppler\bin\pdftotext.exe",
    "$Env:LOCALAPPDATA\Microsoft\WinGet\Packages\oschwartz10612.Poppler_*\poppler-*\Library\bin\pdftotext.exe",
    "$Env:ChocolateyInstall\lib\poppler\tools\poppler*\Library\bin\pdftotext.exe",
    "$Env:USERPROFILE\scoop\apps\poppler\current\Library\bin\pdftotext.exe"
)
$pdftotextPath = Find-ToolPath 'pdftotext.exe' $pdftotextCandidates
if (-not $pdftotextPath -and $AutoInstallRagTools) {
    Install-PopplerIfNeeded
    $pdftotextPath = Find-ToolPath 'pdftotext.exe' $pdftotextCandidates
}

if ($pdftotextPath) {
    $popplerDest = Join-Path $ragToolsDir 'poppler\bin'
    Copy-ToolFiles -SourcePath $pdftotextPath -DestinationDir $popplerDest -CopySiblingFiles
    $ragToolSummary['pdftotext'] = "bundled ($pdftotextPath)"
} else {
    $ragToolSummary['pdftotext'] = 'missing'
    Write-Warning 'pdftotext.exe not found. PDF RAG parsing will remain unavailable until Poppler is installed or pdftotext.exe is placed under build/bin/tools/poppler/bin.'
}

$ragToolSummary['zip'] = 'ok (PowerShell Compress-Archive / ZipFile fallback)'

Write-Host 'RAG runtime support summary:' -ForegroundColor Cyan
$ragToolSummary.GetEnumerator() | ForEach-Object {
    Write-Host ("  - {0}: {1}" -f $_.Key, $_.Value)
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
