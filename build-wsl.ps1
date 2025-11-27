# Build Brimir Linux version in WSL
# Copyright (C) 2025 coredds
# Licensed under GPL-3.0
#
# This script runs the Linux build inside WSL and brings the output back to Windows

param(
    [string]$BuildType = "Release",
    [switch]$Tests = $false,
    [switch]$UseGCC = $false  # Use Clang by default, set this to use GCC instead
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Brimir Linux Build via WSL" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check if WSL is available
$wslCheck = wsl --status 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: WSL is not installed or not configured!" -ForegroundColor Red
    Write-Host "Please install WSL with: wsl --install" -ForegroundColor Yellow
    exit 1
}

Write-Host "✓ WSL is available" -ForegroundColor Green
Write-Host ""

# Get the Windows path and convert to WSL path
$projectPath = (Get-Location).Path
$wslPath = wsl wslpath -a "'$projectPath'"

Write-Host "Project path (Windows): $projectPath" -ForegroundColor Cyan
Write-Host "Project path (WSL):     $wslPath" -ForegroundColor Cyan
Write-Host ""

# Set up environment variables for the build
$env:BUILD_TYPE = $BuildType
$env:ENABLE_TESTS = if ($Tests) { "ON" } else { "OFF" }
$env:USE_CLANG = if ($UseGCC) { "NO" } else { "YES" }

Write-Host "Build configuration:" -ForegroundColor Yellow
Write-Host "  Build Type: $BuildType" -ForegroundColor White
Write-Host "  Tests:      $($env:ENABLE_TESTS)" -ForegroundColor White
Write-Host "  Compiler:   $(if ($UseGCC) { 'GCC' } else { 'Clang (recommended)' })" -ForegroundColor White
Write-Host ""

# Run the Linux build script in WSL
Write-Host "Starting Linux build in WSL..." -ForegroundColor Yellow
Write-Host ""

wsl bash -c "cd '$wslPath' && export BUILD_TYPE='$BuildType' && export ENABLE_TESTS='$($env:ENABLE_TESTS)' && export USE_CLANG='$($env:USE_CLANG)' && ./build.sh"

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "ERROR: Linux build failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "LINUX BUILD SUCCESSFUL!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""

# Check if output exists
$linuxOutput = Join-Path $projectPath "build-linux\lib\brimir_libretro.so"
if (Test-Path $linuxOutput) {
    $fileSize = (Get-Item $linuxOutput).Length / 1MB
    Write-Host "Output:" -ForegroundColor Cyan
    Write-Host "  - $linuxOutput" -ForegroundColor White
    Write-Host "  - Size: $([math]::Round($fileSize, 2)) MB" -ForegroundColor White
} else {
    Write-Host "Warning: Output file not found at expected location" -ForegroundColor Yellow
    Write-Host "Check: build-linux/lib/brimir_libretro.so" -ForegroundColor Yellow
}

Write-Host ""


