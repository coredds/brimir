# Brimir Core - RetroArch Deployment Script
# Automatically builds and deploys the core to RetroArch

param(
    [string]$RetroArchPath = "C:\RetroArch-Win64"
)

Write-Host "Brimir Core - RetroArch Deployment" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan
Write-Host ""

# Configuration
$retroarchBase = $RetroArchPath
$retroarchCores = "$retroarchBase\cores"
$retroarchInfo = "$retroarchBase\info"
$retroarchSystem = "$retroarchBase\system"

# Check if RetroArch is installed
if (-not (Test-Path $retroarchBase)) {
    Write-Host "[ERROR] RetroArch not found at: $retroarchBase" -ForegroundColor Red
    Write-Host ""
    Write-Host "Please install RetroArch first or specify custom path:" -ForegroundColor Yellow
    Write-Host "  .\deploy-retroarch.ps1 -RetroArchPath `"C:\Path\To\RetroArch`"" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Download from: https://www.retroarch.com/" -ForegroundColor Yellow
    exit 1
}

Write-Host "[OK] Found RetroArch at: $retroarchBase" -ForegroundColor Green

# Build the core
Write-Host ""
Write-Host "[BUILD] Building Brimir core..." -ForegroundColor Cyan
cmake --build build --config Release --target brimir_libretro 2>&1 | Out-Null

if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Build failed!" -ForegroundColor Red
    cmake --build build --config Release --target brimir_libretro
    exit 1
}

Write-Host "[OK] Build successful" -ForegroundColor Green

# Deploy core DLL
Write-Host ""
Write-Host "[DEPLOY] Deploying core to RetroArch..." -ForegroundColor Cyan

if (-not (Test-Path $retroarchCores)) {
    New-Item -ItemType Directory -Force -Path $retroarchCores | Out-Null
}

Copy-Item build\bin\Release\brimir_libretro.dll $retroarchCores -Force
Write-Host "  [OK] Copied brimir_libretro.dll" -ForegroundColor Green

# Note: fmt is compiled as header-only, no separate DLL needed
# The core is fully self-contained!

# Deploy core info
if (Test-Path brimir_libretro.info) {
    if (-not (Test-Path $retroarchInfo)) {
        New-Item -ItemType Directory -Force -Path $retroarchInfo | Out-Null
    }
    Copy-Item brimir_libretro.info $retroarchInfo -Force
    Write-Host "  [OK] Copied brimir_libretro.info" -ForegroundColor Green
}

# Check for BIOS files
Write-Host ""
Write-Host "Checking BIOS files..." -ForegroundColor Cyan

$biosFiles = @(
    "sega_101.bin", 
    "mpr-17933.bin", 
    "sega_100.bin", 
    "Sega Saturn BIOS (EUR).bin",
    "Sega Saturn BIOS v1.01 (JAP).bin", 
    "Sega Saturn BIOS v1.00 (JAP).bin"
    # Note: sega1003.bin (JP v1.003) is NOT supported and excluded from deployment
)
$biosFound = 0
$biosMissing = 0

foreach ($bios in $biosFiles) {
    $sourcePath = "tests\fixtures\$bios"
    $destPath = "$retroarchSystem\$bios"
    
    if (Test-Path $sourcePath) {
        if (-not (Test-Path $destPath)) {
            if (-not (Test-Path $retroarchSystem)) {
                New-Item -ItemType Directory -Force -Path $retroarchSystem | Out-Null
            }
            Copy-Item $sourcePath $destPath -Force
            Write-Host "  [OK] Copied $bios to system directory" -ForegroundColor Green
            $biosFound++
        } else {
            Write-Host "  [OK] $bios already in system directory" -ForegroundColor Green
            $biosFound++
        }
    } else {
        Write-Host "  [WARNING]  $bios not found in tests/fixtures/" -ForegroundColor Yellow
        $biosMissing++
    }
}

# Summary
Write-Host ""
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "[SUMMARY] Deployment Summary" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Core Files:" -ForegroundColor White
Write-Host "  [OK] brimir_libretro.dll → $retroarchCores" -ForegroundColor Green
Write-Host "  [OK] brimir_libretro.info → $retroarchInfo" -ForegroundColor Green
Write-Host ""
Write-Host "BIOS Files:" -ForegroundColor White
Write-Host "  [OK] $biosFound BIOS file(s) in system directory" -ForegroundColor Green
if ($biosMissing -gt 0) {
    Write-Host "  [WARNING]  $biosMissing BIOS file(s) missing" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Missing BIOS files should be placed in:" -ForegroundColor Yellow
    Write-Host "  $retroarchSystem" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "[COMPLETE] Deployment complete!" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  1. Launch RetroArch from: $retroarchBase\retroarch.exe" -ForegroundColor White
Write-Host "  2. Go to 'Load Core'" -ForegroundColor White
Write-Host "  3. Select 'Sega - Saturn (Brimir)'" -ForegroundColor White
Write-Host "  4. Load a Saturn game and enjoy!" -ForegroundColor White
Write-Host ""

# Check if RetroArch is running
$retroarchProcess = Get-Process -Name "retroarch" -ErrorAction SilentlyContinue
if ($retroarchProcess) {
    Write-Host "[WARNING]  RetroArch is currently running!" -ForegroundColor Yellow
    Write-Host "   You may need to restart RetroArch to load the new core." -ForegroundColor Yellow
    Write-Host ""
}

