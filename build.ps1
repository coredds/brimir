# Brimir Build Script
# This script sets up the Visual Studio environment and builds the project

param(
    [string]$BuildType = "Release",
    [string]$Arch = "x64"
)

Write-Host "Brimir Build Script" -ForegroundColor Cyan
Write-Host "===================" -ForegroundColor Cyan
Write-Host ""

# Initialize Visual Studio environment
$vswherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswherePath)) {
    Write-Host "ERROR: vswhere.exe not found at: $vswherePath" -ForegroundColor Red
    Write-Host "Trying to build without VS environment setup..." -ForegroundColor Yellow
    $vsPath = $null
} else {
    $vsPath = & $vswherePath -latest -property installationPath
    if (-not $vsPath) {
        Write-Host "WARNING: Visual Studio not found, trying without environment setup" -ForegroundColor Yellow
    }
}

if ($vsPath) {
    Write-Host "Found Visual Studio at: $vsPath" -ForegroundColor Green

    # Import VS environment
    $vcvarsPath = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"
    if (Test-Path $vcvarsPath) {
        Write-Host "Loading Visual Studio environment..." -ForegroundColor Yellow
        & $vcvarsPath -Arch $Arch
    } else {
        Write-Host "WARNING: Cannot find VS DevShell script, continuing anyway" -ForegroundColor Yellow
    }
} else {
    Write-Host "Continuing without Visual Studio environment..." -ForegroundColor Yellow
}

# Configure CMake
Write-Host ""
Write-Host "Configuring CMake..." -ForegroundColor Yellow
cmake -B build -G "Visual Studio 17 2022" -A $Arch

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: CMake configuration failed!" -ForegroundColor Red
    exit 1
}

# Build
Write-Host ""
Write-Host "Building ($BuildType)..." -ForegroundColor Yellow
cmake --build build --config $BuildType

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "BUILD SUCCESSFUL!" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Green
Write-Host ""
Write-Host "Output: build\$BuildType\brimir_libretro.dll" -ForegroundColor Cyan
Write-Host ""

