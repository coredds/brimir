#Requires -Version 5.0

<#
.SYNOPSIS
    Build all Brimir components (core, libretro, tools, JIT)

.DESCRIPTION
    Complete build script with options for Release/Debug, JIT tests, and benchmarks

.PARAMETER BuildType
    Build type: Release or Debug (default: Release)

.PARAMETER WithJIT
    Include JIT test framework

.PARAMETER WithBenchmarks
    Build and run benchmarks after successful build

.PARAMETER Clean
    Clean build directory before building

.EXAMPLE
    .\build-all.ps1
    Standard Release build

.EXAMPLE
    .\build-all.ps1 -BuildType Debug
    Debug build

.EXAMPLE
    .\build-all.ps1 -WithJIT
    Build with JIT test framework

.EXAMPLE
    .\build-all.ps1 -WithBenchmarks
    Build and run performance benchmarks

.EXAMPLE
    .\build-all.ps1 -Clean
    Clean rebuild
#>

param(
    [ValidateSet("Release", "Debug")]
    [string]$BuildType = "Release",
    
    [switch]$WithJIT,
    [switch]$WithBenchmarks,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

# Configuration
$BuildDir = "build"
$VsVcVars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

# Banner
Write-Host ""
Write-Host "=======================================================================" -ForegroundColor Cyan
Write-Host "                    BRIMIR BUILD SYSTEM                                " -ForegroundColor Cyan
Write-Host "=======================================================================" -ForegroundColor Cyan
Write-Host ""

Write-Host "Configuration:" -ForegroundColor Yellow
Write-Host "  Build Type: $BuildType" -ForegroundColor White
Write-Host "  JIT Tests:  $(if ($WithJIT) { 'Enabled' } else { 'Disabled' })" -ForegroundColor White
Write-Host "  Benchmarks: $(if ($WithBenchmarks) { 'Enabled' } else { 'Disabled' })" -ForegroundColor White
Write-Host "  Clean:      $(if ($Clean) { 'Yes' } else { 'No' })" -ForegroundColor White
Write-Host ""

# Step 1: Clean if requested
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
    Write-Host "[OK] Clean complete" -ForegroundColor Green
    Write-Host ""
}

# Step 2: Set up Visual Studio environment and run CMake + Build
Write-Host "Setting up Visual Studio environment and building..." -ForegroundColor Yellow
Write-Host "This may take several minutes..." -ForegroundColor Gray
Write-Host ""

$CMakeArgs = "-S . -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=$BuildType -Wno-dev"
if ($WithJIT) {
    $CMakeArgs += " -DBUILD_JIT_TESTS=ON"
}

$BuildStart = Get-Date

# Create temp batch file for build
$TempBat = Join-Path $env:TEMP "brimir_build_$([guid]::NewGuid().ToString('N')).bat"
$BatchContent = @(
    "@echo off",
    "call `"$VsVcVars`"",
    "if errorlevel 1 exit /b 1",
    "cmake $CMakeArgs",
    "if errorlevel 1 exit /b 1",
    "cmake --build $BuildDir"
)
$BatchContent | Out-File -FilePath $TempBat -Encoding ASCII

# Run the batch file
cmd /c $TempBat
$BuildExitCode = $LASTEXITCODE

# Cleanup
Remove-Item $TempBat -ErrorAction SilentlyContinue

$BuildEnd = Get-Date
$BuildTime = ($BuildEnd - $BuildStart).TotalSeconds

if ($BuildExitCode -ne 0) {
    Write-Host ""
    Write-Host "[FAILED] Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "[OK] Build successful (took $($BuildTime.ToString('F1'))s)" -ForegroundColor Green
Write-Host ""

# Step 3: Check outputs
Write-Host "Checking build outputs..." -ForegroundColor Yellow

# Ninja puts outputs in build\bin, VS puts them in build\bin\Release
$DllPath = "$BuildDir\bin\brimir_libretro.dll"
if (-not (Test-Path $DllPath)) {
    $DllPath = "$BuildDir\bin\$BuildType\brimir_libretro.dll"
}
if (-not (Test-Path $DllPath)) {
    $DllPath = "$BuildDir\brimir_libretro.dll"
}

$Outputs = @{
    "Libretro Core" = $DllPath
}

if ($WithJIT) {
    $Outputs["SH2 Wrapper"] = "$BuildDir\libbrimir-sh2-wrapper.a"
}

$AllFound = $true
foreach ($output in $Outputs.GetEnumerator()) {
    if (Test-Path $output.Value) {
        $Size = (Get-Item $output.Value).Length / 1MB
        Write-Host "  [OK] $($output.Key): $($Size.ToString('F2')) MB" -ForegroundColor Green
    } else {
        Write-Host "  [MISSING] $($output.Key): Not found" -ForegroundColor Red
        $AllFound = $false
    }
}

if (-not $AllFound) {
    Write-Host ""
    Write-Host "[WARNING] Some outputs missing. Build may have been incomplete." -ForegroundColor Yellow
}

# Step 4: Run benchmarks if requested
$BenchmarkPath = "$BuildDir\bin\Release\benchmark_sh2.exe"
if (-not (Test-Path $BenchmarkPath)) {
    $BenchmarkPath = "$BuildDir\bin\benchmark_sh2.exe"
}
if ($WithBenchmarks -and (Test-Path $BenchmarkPath)) {
    Write-Host ""
    Write-Host "=======================================================================" -ForegroundColor Cyan
    Write-Host "RUNNING BENCHMARKS" -ForegroundColor Yellow
    Write-Host "=======================================================================" -ForegroundColor Cyan
    Write-Host ""
    
    & .\tools\run_benchmarks.ps1 -Save
}

# Summary
Write-Host ""
Write-Host "=======================================================================" -ForegroundColor Cyan
Write-Host "BUILD COMPLETE" -ForegroundColor Green
Write-Host "=======================================================================" -ForegroundColor Cyan

Write-Host ""
Write-Host "Quick Start:" -ForegroundColor Yellow
Write-Host "  Libretro DLL: $DllPath" -ForegroundColor White
Write-Host "  Run benchmarks: .\tools\run_benchmarks.ps1" -ForegroundColor White
Write-Host ""
