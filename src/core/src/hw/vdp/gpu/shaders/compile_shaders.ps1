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

Write-Host "Compiling VDP1 shaders..." -ForegroundColor Cyan

# Compile vertex shader
& $glslc -fshader-stage=vertex vdp1_vertex.glsl -o vdp1_vertex.spv
if ($LASTEXITCODE -eq 0) {
    Write-Host "✓ vdp1_vertex.spv" -ForegroundColor Green
} else {
    Write-Error "Failed to compile vdp1_vertex.glsl"
    exit 1
}

# Compile fragment shader
& $glslc -fshader-stage=fragment vdp1_fragment.glsl -o vdp1_fragment.spv
if ($LASTEXITCODE -eq 0) {
    Write-Host "✓ vdp1_fragment.spv" -ForegroundColor Green
} else {
    Write-Error "Failed to compile vdp1_fragment.glsl"
    exit 1
}

Write-Host ""
Write-Host "All shaders compiled successfully!" -ForegroundColor Green

