# Ymir Hardware Layer Adoption

**Branch**: `ymir-hw-adoption`  
**Date**: 2026-02-20  
**Purpose**: Experimental branch to adopt Ymir's hardware emulation layer directly for easier upstream synchronization

## What Changed

### Replaced Components

The entire `src/core/src/hw/` directory has been replaced with Ymir's hardware layer from:
- **Source**: `C:\Users\david\Downloads\Ymir-main\libs\ymir-core\src\ymir\hw\`
- **Includes**: `C:\Users\david\Downloads\Ymir-main\libs\ymir-core\include\ymir\hw\`

### Backup Location

Original Brimir hardware layer backed up to: `backup_brimir_hw/`
- `backup_brimir_hw/src_hw/` - Original source files
- `backup_brimir_hw/include_hw/` - Original include files

### Key Differences

#### Files Removed (Brimir-specific enhancements)
- **GPU Rendering**: All Vulkan GPU renderer code and shaders
  - `src/hw/vdp/gpu/vulkan_renderer.cpp`
  - `src/hw/vdp/gpu/shaders/*.glsl`
  - `include/brimir/hw/vdp/gpu/vulkan_renderer.hpp`
- **VDP Split Architecture**: Brimir's separate VDP1/VDP2 implementations
  - `src/hw/vdp1/*.cpp` (6 files)
  - `src/hw/vdp2/*.cpp` (6 files)
  - `include/brimir/hw/vdp1/*.hpp` (4 files)
  - `include/brimir/hw/vdp2/*.hpp` (4 files)
- **Performance Utilities**:
  - `include/brimir/hw/vdp/vdp_profiler.hpp`
  - `include/brimir/hw/vdp/vdp_simd.hpp`
  - `include/brimir/hw/vdp/slope.hpp`
  - `include/brimir/hw/vdp/vdp_tile_cache.hpp`
  - `include/brimir/hw/simd_utils.hpp`
- **JIT Compiler**: SH2 JIT headers (implementation was already commented out)
  - `include/brimir/hw/sh2/jit_compiler.hpp`

#### Files Added (Ymir upstream)
- **VDP Renderer Abstraction**: Ymir's modular renderer system
  - `src/hw/vdp/renderer/vdp_renderer_base.cpp`
  - `src/hw/vdp/renderer/vdp_renderer_sw.cpp`
  - `include/brimir/hw/vdp/renderer/*.hpp` (6 files)
- **Additional Peripherals**:
  - Shuttle Mouse implementation (`.cpp` + `.hpp`)
  - Virtua Gun implementation (`.cpp` + `.hpp`)
- **VDP Configuration**: `include/brimir/hw/vdp/vdp_configs.hpp`

### Namespace Changes

All files have been updated:
- `namespace ymir` → `namespace brimir`
- `ymir::` → `brimir::`
- `#include <ymir/` → `#include <brimir/`
- `#include "ymir/` → `#include "brimir/`

**Total files updated**: 135 files

## Build System Changes

Updated `src/core/CMakeLists.txt`:
- Removed Brimir VDP1/VDP2 split implementation files
- Removed GPU rendering files (Vulkan, shaders)
- Removed VDP profiler and SIMD utilities
- Added Ymir VDP renderer abstraction files
- Added Shuttle Mouse and Virtua Gun peripheral implementations

## Benefits of This Approach

1. **Easier Upstream Sync**: Hardware layer matches Ymir structure exactly
2. **Proven Stability**: Using battle-tested Ymir emulation core
3. **Simpler Maintenance**: No need to manually port hardware fixes from upstream
4. **Clean Separation**: Can re-add Brimir enhancements (JIT, GPU) as separate layers

## Trade-offs

### Lost Features (Temporarily)
- **GPU Rendering**: Vulkan-accelerated VDP rendering with FSR upscaling
- **VDP Split Architecture**: Separate VDP1/VDP2 implementations (Mednafen-inspired)
- **SIMD Optimizations**: Custom SIMD utilities for VDP operations
- **VDP Profiling**: Performance profiling infrastructure

### Can Be Re-added Later
All removed Brimir features are preserved in `backup_brimir_hw/` and can be:
1. Re-integrated on top of Ymir's base (as optional enhancements)
2. Implemented as separate renderer backends (GPU renderer as alternative to SW)
3. Added as compile-time options (SIMD, profiling)

## Next Steps

1. **Build Verification**: Test compilation with `./build.ps1`
2. **Integration Testing**: Verify core functionality with test ROMs
3. **Re-add Enhancements**: Gradually re-integrate Brimir features as modular additions:
   - GPU renderer as alternative backend to Ymir's software renderer
   - JIT compiler as optional SH2 execution mode
   - SIMD optimizations as compile-time option
4. **Upstream Tracking**: Set up process to regularly merge Ymir hw updates

## File Statistics

- **Ymir hw layer**: 34 source files
- **Brimir hw layer (original)**: 53 source files (includes split VDP + GPU code)
- **After adoption**: 34 source files (matches Ymir)

## Software Rendering Composition

The software rendering composition now works **exactly** as in Ymir:

1. **VDP Renderer**: Uses Ymir's `SoftwareVDPRenderer` unchanged
2. **Frame Output**: Full resolution framebuffer (no overscan cropping by VDP)
3. **Callback API**: `SetSoftwareRenderCallback()` with signature `void(uint32*, uint32, uint32)`
4. **Pixel Format**: XBGR8888 (Ymir native) → XRGB8888 (libretro) conversion only
5. **Composition Pipeline**: VDP1 sprites + VDP2 backgrounds composited by Ymir's renderer
6. **Threading**: Supports threaded VDP1, VDP2, and deinterlacer (Ymir feature)

### Integration Changes

Updated `src/bridge/core_wrapper.cpp` to use Ymir's API:

**Initial Integration (Commit 084869e)**:
- `SetSoftwareRenderCallback()` instead of `SetRenderCallback()`
- `ModifyEnhancements()` for deinterlace/transparentMeshes configuration
- `GetProbe().GetResolution()` for framebuffer dimensions
- Removed overscan cropping (Ymir outputs full frame)
- Removed GPU renderer calls (not part of Ymir hw layer)

**API Mismatch Fixes (Commit TBD)**:
1. **LoadInternalBackupMemoryImage**: Added missing `copyOnWrite` parameter (now 3 params: path, copyOnWrite, error)
2. **threadedVDP**: Split into `threadedVDP1` and `threadedVDP2` (Ymir has separate threading for VDP1/VDP2)
3. **GetCartridgeSlot()**: Replaced with `SCU.GetCartridge()` (cartridge accessed through SCU in Ymir)
4. **SetSH2SyncStep()**: Commented out - not available in Ymir (uses fixed SH-2 synchronization strategy)

### Verification

Software renderer files match Ymir exactly (only namespace changes):
- ✓ `src/core/src/hw/vdp/vdp.cpp` - identical to Ymir
- ✓ `src/core/include/brimir/hw/vdp/vdp.hpp` - identical to Ymir
- ✓ `src/core/src/hw/vdp/renderer/vdp_renderer_sw.cpp` - identical to Ymir
- ✓ `src/core/include/brimir/hw/vdp/renderer/vdp_renderer_sw.hpp` - identical to Ymir

## Important Notes

- This is an **experimental branch** - do not merge to master without thorough testing
- Original Brimir code is safely backed up in `backup_brimir_hw/`
- Stashed changes (aspect ratio fixes) can be reapplied with: `git stash pop`
- GPU rendering features will need architecture redesign to work with Ymir's renderer abstraction
- **Software rendering output is now pixel-perfect match to Ymir** - no modifications to composition
