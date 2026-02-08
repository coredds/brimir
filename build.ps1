# Brimir Build Script
# This script sets up the Visual Studio environment and builds the project

param(
    [string]$BuildType = "Release",
    [string]$Arch = "x64",
    [string]$Generator = "Ninja",
    [switch]$Clean,
    [switch]$Reconfigure,
    [switch]$Target,
    [switch]$GPU = $true,
    [switch]$NoGPU
)

$ErrorActionPreference = "Stop"

# NoGPU overrides GPU
if ($NoGPU) { $GPU = $false }

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "    Brimir Build Script" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Parse target
$BuildTarget = if ($Target) { $Target } else { "brimir_libretro" }

# Clean build directory if requested
if ($Clean) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    if (Test-Path "build") {
        Remove-Item -Recurse -Force "build"
        Write-Host "Build directory cleaned." -ForegroundColor Green
    }
    $Reconfigure = $true
}

# Initialize Visual Studio environment
$vswherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswherePath)) {
    Write-Host "WARNING: vswhere.exe not found" -ForegroundColor Yellow
    Write-Host "Trying to build without VS environment setup..." -ForegroundColor Yellow
    $vsPath = $null
} else {
    $vsPath = & $vswherePath -latest -property installationPath
    if (-not $vsPath) {
        Write-Host "WARNING: Visual Studio not found" -ForegroundColor Yellow
    }
}

if ($vsPath) {
    Write-Host "Found Visual Studio at: $vsPath" -ForegroundColor Green

    # Import VS environment for Ninja builds
    if ($Generator -eq "Ninja") {
        Write-Host "Loading Visual Studio environment for Ninja..." -ForegroundColor Yellow
        
        # Use vcvarsall.bat for reliable environment setup
        $vcvarsallPath = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path $vcvarsallPath) {
            # Get the environment variables after running vcvarsall
            $archArg = if ($Arch -eq "x64") { "x64" } else { "x86" }
            
            $output = & cmd /c "`"$vcvarsallPath`" $archArg > nul && set"
            
            foreach ($line in $output) {
                if ($line -match "^([^=]+)=(.*)$") {
                    [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
                }
            }
            
            Write-Host "Visual Studio environment loaded successfully." -ForegroundColor Green
        } else {
            Write-Host "WARNING: vcvarsall.bat not found, CMake may not find compiler" -ForegroundColor Yellow
        }
    }
} else {
    Write-Host "WARNING: Visual Studio not found" -ForegroundColor Yellow
    if ($Generator -eq "Ninja") {
        Write-Host "  Ninja requires a C++ compiler to be available" -ForegroundColor Yellow
        Write-Host "  Try using -Generator 'Visual Studio 17 2022' instead" -ForegroundColor Yellow
    }
}

# Configure CMake if needed
if ($Reconfigure -or -not (Test-Path "build/CMakeCache.txt")) {
    Write-Host ""
    Write-Host "Configuring CMake..." -ForegroundColor Yellow
    Write-Host "  Generator: $Generator" -ForegroundColor Cyan
    Write-Host "  Build Type: $BuildType" -ForegroundColor Cyan
    Write-Host "  Architecture: $Arch" -ForegroundColor Cyan
    Write-Host ""
    
    $cmakeArgs = @(
        "-B", "build",
        "-G", $Generator,
        "-DCMAKE_BUILD_TYPE=$BuildType"
    )
    
    # Add architecture for Visual Studio generators
    if ($Generator -like "Visual Studio*") {
        $cmakeArgs += @("-A", $Arch)
    }
    
    # GPU (Vulkan) rendering
    if ($GPU) {
        Write-Host "  GPU Rendering: ENABLED (Vulkan)" -ForegroundColor Cyan
        $cmakeArgs += @("-DBRIMIR_GPU_VULKAN=ON")
    } else {
        Write-Host "  GPU Rendering: Disabled (use default or remove -NoGPU to enable)" -ForegroundColor DarkGray
    }
    
    & cmake @cmakeArgs
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "ERROR: CMake configuration failed!" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "Configuration complete." -ForegroundColor Green
}

# Build
Write-Host ""
Write-Host "Building target: $BuildTarget ($BuildType)..." -ForegroundColor Yellow
Write-Host ""

$buildArgs = @(
    "--build", "build",
    "--config", $BuildType,
    "--target", $BuildTarget
)

# Add parallel build flag for Ninja
if ($Generator -eq "Ninja") {
    $buildArgs += @("--parallel")
}

& cmake @buildArgs

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "ERROR: Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "    BUILD SUCCESSFUL!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""

# Find and display output files
$outputPatterns = @(
    "build\bin\brimir_libretro.dll",
    "build\bin\$BuildType\brimir_libretro.dll",
    "build\lib\brimir_libretro.dll"
)

$foundOutput = $false
foreach ($pattern in $outputPatterns) {
    if (Test-Path $pattern) {
        Write-Host "Output: $pattern" -ForegroundColor Cyan
        $foundOutput = $true
        break
    }
}

if (-not $foundOutput) {
    Write-Host "Note: Output DLL location may vary based on generator" -ForegroundColor Yellow
}

Write-Host ""

