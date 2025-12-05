/**
 * @file vdp2.cpp
 * @brief VDP2 Core Implementation
 * 
 * Based on Mednafen's VDP2 implementation with adaptations for Brimir.
 * 
 * Copyright (C) 2015-2019 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp2/vdp2.hpp>
#include <brimir/hw/vdp2/vdp2_layers.hpp>
#include <brimir/hw/vdp2/vdp2_helpers.hpp>
#include <brimir/hw/vdp2/vdp2_render.hpp>
#include <brimir/util/dev_log.hpp>

#include <cstring>

namespace brimir::vdp2 {

// =============================================================================
// Dev Log Groups
// =============================================================================

namespace grp {
    struct vdp2 {
        static constexpr bool enabled = true;
        static constexpr devlog::Level level = devlog::level::debug;
        static constexpr std::string_view name = "VDP2";
    };
    
    struct vdp2_regs : public vdp2 {
        static constexpr std::string_view name = "VDP2-Regs";
    };
    
    struct vdp2_render : public vdp2 {
        static constexpr std::string_view name = "VDP2-Render";
    };
    
    struct vdp2_layer : public vdp2 {
        static constexpr std::string_view name = "VDP2-Layer";
    };
}

// =============================================================================
// Global State Definitions
// =============================================================================

// Memory
uint16 VRAM[VRAM_SIZE / 2];
uint16 CRAM[CRAM_SIZE / 2];

// Registers
VDP2Regs Regs;

// Rotation parameters
RotationParams RotParams[2];

// Window state
WindowState Windows[2];

// Timing
sint32 VCounter = 0;
sint32 HCounter = 0;
bool VBlankOut = false;
bool HBlankOut = false;
bool PAL = false;

// Display
uint32 HRes = 320;
uint32 VRes = 224;
uint32 VResBase = 224;
bool CurrentField = false;
bool InterlaceDouble = false;

// VDP1 Framebuffer
const uint16* VDP1Framebuffer = nullptr;
uint32 VDP1FBWidth = 0;
uint32 VDP1FBHeight = 0;

// Internal state
static sint64 LastTimestamp = 0;
static uint64 LayerEnableMask = 0xFFFFFFFF;

// =============================================================================
// Core Functions
// =============================================================================

void Init(bool isPAL) {
    devlog::info<grp::vdp2>("Initializing VDP2 ({})", isPAL ? "PAL" : "NTSC");
    
    PAL = isPAL;
    
    // Clear memory
    std::memset(VRAM, 0, sizeof(VRAM));
    std::memset(CRAM, 0, sizeof(CRAM));
    
    // Initialize state
    Reset(true);
}

void Reset(bool poweringUp) {
    devlog::info<grp::vdp2>("Resetting VDP2 (power={})", poweringUp);
    
    // Clear registers
    std::memset(&Regs, 0, sizeof(Regs));
    
    // Default display settings
    Regs.displayOn = false;
    Regs.borderMode = false;
    Regs.hRes = HResMode::Normal;
    Regs.vRes = VResMode::Lines224;
    Regs.interlace = InterlaceMode::None;
    Regs.cramMode = CRAMMode::RGB555_1024;
    
    // Reset rotation parameters
    std::memset(RotParams, 0, sizeof(RotParams));
    
    // Reset windows
    std::memset(Windows, 0, sizeof(Windows));
    
    // Reset timing
    VCounter = 0;
    HCounter = 0;
    VBlankOut = true;
    HBlankOut = false;
    LastTimestamp = 0;
    
    // Default resolution
    HRes = 320;
    VRes = PAL ? 240 : 224;
}

void Kill() {
    devlog::info<grp::vdp2>("Shutting down VDP2");
}

sint64 Update(sint64 timestamp) {
    // TODO: Implement timing update
    // This will be called to advance VDP2 state
    
    sint64 delta = timestamp - LastTimestamp;
    LastTimestamp = timestamp;
    
    // For now, just return next event time
    return timestamp + 1000;
}

void StartFrame(bool clock28m) {
    devlog::trace<grp::vdp2>("Starting frame (28MHz={}, field={})", clock28m, CurrentField);
    
    // Reset line counter
    VCounter = 0;
    
    // Toggle field for interlaced modes (like Mednafen)
    // Field alternates each frame: 0, 1, 0, 1, ...
    if (InterlaceDouble) {
        CurrentField = !CurrentField;
    } else {
        CurrentField = false;
    }
    
    // Update SIMD-optimized color cache
    UpdateColorCache();
    
    // Resolution is now updated by TVMD register writes
    // No need to recalculate here
}

void EndFrame() {
    devlog::trace<grp::vdp2>("Ending frame");
}

void SetVDP1Framebuffer(const uint16* data, uint32 width, uint32 height) {
    VDP1Framebuffer = data;
    VDP1FBWidth = width;
    VDP1FBHeight = height;
}

void ClearVDP1Framebuffer() {
    VDP1Framebuffer = nullptr;
    VDP1FBWidth = 0;
    VDP1FBHeight = 0;
}

void SetLayerEnableMask(uint64 mask) {
    LayerEnableMask = mask;
}

} // namespace brimir::vdp2


