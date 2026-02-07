#!/usr/bin/env pwsh
# Compile GLSL shaders to SPIR-V for Vulkan

$VulkanSDK = $env:VULKAN_SDK
if (-not $VulkanSDK) {
    Write-Error "VULKAN_SDK environment variable not set!"
    exit 1
}

$glslc = "$VulkanSDK\Bin\glslc.exe"
if (-not (Test-Path $glslc)) {
    Write-Error "glslc not found at: $glslc"
    exit 1
}

Write-Host "Compiling shaders..." -ForegroundColor Cyan

# Upscale shaders
Write-Host "Upscale shaders:" -ForegroundColor Yellow
& $glslc -fshader-stage=vertex upscale.vert.glsl -o upscale.vert.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ upscale.vert.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile upscale.vert.glsl"; exit 1 }

& $glslc -fshader-stage=fragment upscale.frag.glsl -o upscale.frag.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ upscale.frag.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile upscale.frag.glsl"; exit 1 }

# FXAA shaders (reuses upscale.vert.glsl as vertex shader)
Write-Host "FXAA shaders:" -ForegroundColor Yellow
& $glslc -fshader-stage=fragment fxaa.frag.glsl -o fxaa.frag.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ fxaa.frag.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile fxaa.frag.glsl"; exit 1 }

# FSR 1.0 RCAS shaders (reuses upscale.vert.glsl as vertex shader)
Write-Host "FSR RCAS shaders:" -ForegroundColor Yellow
& $glslc -fshader-stage=fragment fsr_rcas.frag.glsl -o fsr_rcas.frag.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ fsr_rcas.frag.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile fsr_rcas.frag.glsl"; exit 1 }

Write-Host ""
Write-Host "All shaders compiled successfully!" -ForegroundColor Green
