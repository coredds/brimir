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

Write-Host "Compiling VDP shaders..." -ForegroundColor Cyan

# VDP1 basic shaders
Write-Host "VDP1 basic shaders:" -ForegroundColor Yellow
& $glslc -fshader-stage=vertex vdp1_vertex.glsl -o vdp1_vertex.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ vdp1_vertex.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile vdp1_vertex.glsl"; exit 1 }

& $glslc -fshader-stage=fragment vdp1_fragment.glsl -o vdp1_fragment.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ vdp1_fragment.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile vdp1_fragment.glsl"; exit 1 }

# VDP1 textured shaders
Write-Host "VDP1 textured shaders:" -ForegroundColor Yellow
& $glslc -fshader-stage=vertex vdp1_textured_vertex.glsl -o vdp1_textured_vertex.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ vdp1_textured_vertex.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile vdp1_textured_vertex.glsl"; exit 1 }

& $glslc -fshader-stage=fragment vdp1_textured_fragment.glsl -o vdp1_textured_fragment.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ vdp1_textured_fragment.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile vdp1_textured_fragment.glsl"; exit 1 }

# Compositor shaders
Write-Host "Compositor shaders:" -ForegroundColor Yellow
& $glslc -fshader-stage=vertex compositor.vert.glsl -o compositor.vert.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ compositor.vert.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile compositor.vert.glsl"; exit 1 }

& $glslc -fshader-stage=fragment compositor.frag.glsl -o compositor.frag.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ compositor.frag.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile compositor.frag.glsl"; exit 1 }

# NBG layer shaders
Write-Host "NBG layer shaders:" -ForegroundColor Yellow
& $glslc -fshader-stage=vertex nbg_layer.vert.glsl -o nbg_layer.vert.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ nbg_layer.vert.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile nbg_layer.vert.glsl"; exit 1 }

& $glslc -fshader-stage=fragment nbg_layer.frag.glsl -o nbg_layer.frag.spv
if ($LASTEXITCODE -eq 0) { Write-Host "  ✓ nbg_layer.frag.spv" -ForegroundColor Green }
else { Write-Error "Failed to compile nbg_layer.frag.glsl"; exit 1 }

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

Write-Host ""
Write-Host "All shaders compiled successfully!" -ForegroundColor Green
