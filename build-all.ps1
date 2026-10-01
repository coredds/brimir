#Requires -Version 5.0

<#
.SYNOPSIS
    Build all Brimir components (core, libretro, tools)

.DESCRIPTION
    Complete build script with options for Release/Debug

.PARAMETER BuildType
    Build type: Release or Debug (default: Release)

.PARAMETER Clean
    Clean build directory before building

.EXAMPLE
    .\build-all.ps1
    Standard Release build

.EXAMPLE
    .\build-all.ps1 -BuildType Debug
    Debug build

.EXAMPLE
    .\build-all.ps1 -Clean
    Clean rebuild
#>

param(
    [ValidateSet("Release", "Debug")]
    [string]$BuildType = "Release",
    
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

# Configuration
$BuildDir = "build"
$CMakeGenerator = "Visual Studio 17 2022"

# Banner
Write-Host "`n╔═══════════════════════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║                                                                       ║" -ForegroundColor Cyan
Write-Host "║                    BRIMIR BUILD SYSTEM                                ║" -ForegroundColor Cyan
Write-Host "║                                                                       ║" -ForegroundColor Cyan
Write-Host "╚═══════════════════════════════════════════════════════════════════════╝`n" -ForegroundColor Cyan

Write-Host "Configuration:" -ForegroundColor Yellow
Write-Host "  Build Type: $BuildType" -ForegroundColor White
Write-Host "  Clean:      $(if ($Clean) { 'Yes' } else { 'No' })`n" -ForegroundColor White

# Step 1: Clean if requested
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
    Write-Host "✅ Clean complete`n" -ForegroundColor Green
}

# Step 2: CMake Configure
Write-Host "Configuring CMake..." -ForegroundColor Yellow

$CMakeArgs = @(
    "-S", ".",
    "-B", $BuildDir,
    "-G", $CMakeGenerator,
    "-DCMAKE_BUILD_TYPE=$BuildType"
)

& cmake @CMakeArgs 2>&1 | Out-Null

if ($LASTEXITCODE -ne 0) {
    Write-Host "❌ CMake configuration failed!" -ForegroundColor Red
    exit 1
}

Write-Host "✅ Configuration complete`n" -ForegroundColor Green

# Step 3: Build
Write-Host "Building Brimir ($BuildType)..." -ForegroundColor Yellow
Write-Host "This may take several minutes...`n" -ForegroundColor Gray

$BuildStart = Get-Date

cmake --build $BuildDir --config $BuildType 2>&1 | ForEach-Object {
    if ($_ -match "error C") {
        Write-Host $_ -ForegroundColor Red
    } elseif ($_ -match "warning C") {
        Write-Host $_ -ForegroundColor Yellow
    } elseif ($_ -match "Building") {
        Write-Host $_ -ForegroundColor Cyan
    }
}

$BuildEnd = Get-Date
$BuildTime = ($BuildEnd - $BuildStart).TotalSeconds

if ($LASTEXITCODE -ne 0) {
    Write-Host "`n❌ Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host "`n✅ Build successful (took $($BuildTime.ToString('F1'))s)`n" -ForegroundColor Green

# Step 4: Check outputs
Write-Host "Checking build outputs..." -ForegroundColor Yellow

$Outputs = @{
    "Libretro Core" = "$BuildDir\bin\$BuildType\brimir_libretro.dll"
    "Frame Benchmark" = "$BuildDir\bin\$BuildType\brimir_bench.exe"
}

$AllFound = $true
foreach ($output in $Outputs.GetEnumerator()) {
    if (Test-Path $output.Value) {
        $Size = (Get-Item $output.Value).Length / 1MB
        Write-Host "  ✅ $($output.Key): $($Size.ToString('F2')) MB" -ForegroundColor Green
    } else {
        Write-Host "  ❌ $($output.Key): Not found" -ForegroundColor Red
        $AllFound = $false
    }
}

if (-not $AllFound) {
    Write-Host "`n⚠️  Some outputs missing. Build may have been incomplete." -ForegroundColor Yellow
}

# Summary
Write-Host "`n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" -ForegroundColor Cyan
Write-Host "BUILD COMPLETE" -ForegroundColor Green
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" -ForegroundColor Cyan

Write-Host "`nQuick Start:" -ForegroundColor Yellow
Write-Host "  • Libretro DLL: $BuildDir\bin\$BuildType\brimir_libretro.dll" -ForegroundColor White
Write-Host "  • Benchmark: $BuildDir\bin\$BuildType\brimir_bench.exe --bios <file> --game <file>" -ForegroundColor White
Write-Host "  • Documentation: README.md" -ForegroundColor White
Write-Host "`n"

