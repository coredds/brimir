# Build Brimir for both Windows and Linux
# Copyright (C) 2025 coredds
# Licensed under GPL-3.0
#
# This script builds both Windows (native) and Linux (via WSL) versions

param(
    [string]$BuildType = "Release",
    [switch]$WindowsOnly = $false,
    [switch]$LinuxOnly = $false,
    [switch]$Tests = $false
)

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Brimir Multi-Platform Build" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Determine what to build
$buildWindows = !$LinuxOnly
$buildLinux = !$WindowsOnly

if ($buildWindows -and $buildLinux) {
    Write-Host "Building for: Windows + Linux" -ForegroundColor Green
} elseif ($buildWindows) {
    Write-Host "Building for: Windows only" -ForegroundColor Green
} else {
    Write-Host "Building for: Linux only" -ForegroundColor Green
}

Write-Host "Build Type: $BuildType" -ForegroundColor White
Write-Host "Tests: $(if ($Tests) { 'Enabled' } else { 'Disabled' })" -ForegroundColor White
Write-Host ""

$startTime = Get-Date
$buildResults = @{}

# Build Windows version
if ($buildWindows) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "Building Windows Version" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""
    
    try {
        & .\build.ps1 -BuildType $BuildType
        if ($LASTEXITCODE -eq 0) {
            $buildResults["Windows"] = "✓ Success"
            Write-Host ""
            Write-Host "✓ Windows build completed successfully" -ForegroundColor Green
        } else {
            $buildResults["Windows"] = "✗ Failed"
            Write-Host ""
            Write-Host "✗ Windows build failed" -ForegroundColor Red
        }
    } catch {
        $buildResults["Windows"] = "✗ Failed (Exception: $($_.Exception.Message))"
        Write-Host ""
        Write-Host "✗ Windows build failed with exception" -ForegroundColor Red
        Write-Host $_.Exception.Message -ForegroundColor Red
    }
}

# Build Linux version via WSL
if ($buildLinux) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "Building Linux Version (via WSL)" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""
    
    try {
        $testFlag = if ($Tests) { "-Tests" } else { "" }
        & .\build-wsl.ps1 -BuildType $BuildType $testFlag
        if ($LASTEXITCODE -eq 0) {
            $buildResults["Linux"] = "✓ Success"
            Write-Host ""
            Write-Host "✓ Linux build completed successfully" -ForegroundColor Green
        } else {
            $buildResults["Linux"] = "✗ Failed"
            Write-Host ""
            Write-Host "✗ Linux build failed" -ForegroundColor Red
        }
    } catch {
        $buildResults["Linux"] = "✗ Failed (Exception: $($_.Exception.Message))"
        Write-Host ""
        Write-Host "✗ Linux build failed with exception" -ForegroundColor Red
        Write-Host $_.Exception.Message -ForegroundColor Red
    }
}

# Summary
$endTime = Get-Date
$duration = $endTime - $startTime

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Build Summary" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

foreach ($platform in $buildResults.Keys | Sort-Object) {
    $result = $buildResults[$platform]
    if ($result -like "*Success*") {
        Write-Host "$platform : $result" -ForegroundColor Green
    } else {
        Write-Host "$platform : $result" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "Total time: $($duration.ToString('mm\:ss'))" -ForegroundColor Cyan
Write-Host ""

# List outputs
Write-Host "Build outputs:" -ForegroundColor Yellow
Write-Host ""

if ($buildWindows -and (Test-Path "build\bin\Release\brimir_libretro.dll")) {
    $winSize = (Get-Item "build\bin\Release\brimir_libretro.dll").Length / 1MB
    Write-Host "  Windows: build\bin\Release\brimir_libretro.dll" -ForegroundColor White
    Write-Host "           Size: $([math]::Round($winSize, 2)) MB" -ForegroundColor Gray
    Write-Host "           Info: brimir_libretro.info" -ForegroundColor Gray
}

if ($buildLinux -and (Test-Path "build-linux\lib\libbrimir_libretro.so")) {
    $linuxSize = (Get-Item "build-linux\lib\libbrimir_libretro.so").Length / 1MB
    Write-Host "  Linux:   build-linux\lib\libbrimir_libretro.so" -ForegroundColor White
    Write-Host "           Size: $([math]::Round($linuxSize, 2)) MB" -ForegroundColor Gray
    Write-Host "           Info: brimir_libretro.info" -ForegroundColor Gray
}

Write-Host ""

# Check for failures
$failures = $buildResults.Values | Where-Object { $_ -like "*Failed*" }
if ($failures) {
    Write-Host "⚠ Some builds failed. Check the output above for details." -ForegroundColor Yellow
    exit 1
} else {
    Write-Host "✓ All builds completed successfully!" -ForegroundColor Green
    exit 0
}


