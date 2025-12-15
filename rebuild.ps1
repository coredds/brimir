# Quick Rebuild Script
# Use this for incremental builds after the initial configuration

param(
    [string]$BuildType = "Release",
    [string]$Target = "brimir_libretro"
)

Write-Host ""
Write-Host "Quick Rebuild - $Target ($BuildType)" -ForegroundColor Cyan
Write-Host ""

if (-not (Test-Path "build/build.ninja")) {
    Write-Host "ERROR: Build not configured. Run .\build.ps1 first!" -ForegroundColor Red
    exit 1
}

# Just build without reconfiguring
ninja -C build $Target

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Build complete!" -ForegroundColor Green
Write-Host ""

