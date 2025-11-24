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
$vsPath = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
if (-not $vsPath) {
    Write-Host "ERROR: Visual Studio not found!" -ForegroundColor Red
    exit 1
}

Write-Host "Found Visual Studio at: $vsPath" -ForegroundColor Green

# Import VS environment
$vcvarsPath = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"
if (Test-Path $vcvarsPath) {
    Write-Host "Loading Visual Studio environment..." -ForegroundColor Yellow
    & $vcvarsPath -Arch $Arch
} else {
    Write-Host "ERROR: Cannot find VS DevShell script!" -ForegroundColor Red
    exit 1
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

