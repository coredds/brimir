#pragma once

/**
 * @file vdp1.hpp
 * @brief VDP1 (Video Display Processor 1) - Sprite and Polygon Engine
 * 
 * Based on Mednafen's VDP1 implementation with adaptations for Brimir.
 * Architecture inspired by beetle-saturn-libretro/mednafen/ss/vdp1.*
 * 
 * Copyright (C) 2015-2017 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 * 
 * VDP1 handles sprite and polygon rendering to framebuffers.
 * Features: scaled sprites, distorted sprites, polygons, lines, Gouraud shading.
 */

#include <brimir/core/types.hpp>
#include <brimir/core/scheduler.hpp>
#include <brimir/sys/bus.hpp>

#include <array>
#include <cstdint>

namespace brimir::vdp1 {

// ============================================================================
// Constants
// ============================================================================

constexpr uint32 VRAM_SIZE = 0x80000;      // 512KB VRAM
constexpr uint32 FB_SIZE = 0x40000;        // 256KB per framebuffer
constexpr uint32 VRAM_MASK = 0x7FFFF;      // VRAM address mask
constexpr uint32 FB_MASK = 0x3FFFF;        // FB address mask

// Timing constants
constexpr sint32 UPDATE_TIMING_GRAN = 263;  // Cycles per update when drawing
constexpr sint32 IDLE_TIMING_GRAN = 1019;   // Cycles per update when idle

// ============================================================================
// Register Definitions
// ============================================================================

// TVMR - TV Mode Register
enum TVMRFlags : uint8 {
    TVMR_8BPP   = 0x01,  // 8-bit per pixel mode
    TVMR_ROTATE = 0x02,  // Rotation enabled
    TVMR_HDTV   = 0x04,  // HDTV mode (512x256)
    TVMR_VBE    = 0x08   // VBlank erase enable
};

// FBCR - Framebuffer Change Register
enum FBCRFlags : uint8 {
    FBCR_FCT = 0x01,  // Frame buffer change trigger
    FBCR_FCM = 0x02,  // Frame buffer change mode (0=manual, 1=auto)
    FBCR_DIL = 0x04,  // Double interlace draw line (0=even, 1=odd)
    FBCR_DIE = 0x08,  // Double interlace enable
    FBCR_EOS = 0x10   // Even/Odd coordinate select
};

// PTMR - Plot Trigger Register
enum PTMRFlags : uint8 {
    PTMR_PTM_MASK = 0x03  // Plot trigger mode
};

// EDSR - End Status Register
enum EDSRFlags : uint8 {
    EDSR_CEF = 0x02  // Command end flag
};

// ============================================================================
// VDP1 State
// ============================================================================

struct VDP1State {
    // VRAM and framebuffers
    std::array<uint16, 0x40000> VRAM;      // 512KB VRAM (as 16-bit words)
    std::array<uint16, 0x20000> FB[2];     // Two 256KB framebuffers
    bool FBDrawWhich;                       // Which FB is currently being drawn to (0 or 1)
    
    // Registers
    uint8 TVMR;   // TV Mode Register
    uint8 FBCR;   // Framebuffer Change Register
    uint8 PTMR;   // Plot Trigger Register
    uint8 EDSR;   // End Status Register
    
    uint16 LOPR;  // Last Operation Register
    
    // Erase/Write registers
    uint16 EWDR;  // Erase/Write Data
    uint16 EWLR;  // Erase/Write Upper Left
    uint16 EWRR;  // Erase/Write Lower Right
    
    // Clipping
    sint32 SysClipX, SysClipY;
    sint32 UserClipX0, UserClipY0, UserClipX1, UserClipY1;
    
    // Local coordinate system
    sint32 LocalX, LocalY;
    
    // Drawing state
    uint32 CurCommandAddr;
    sint32 RetCommandAddr;
    bool DrawingActive;
    sint32 CycleCounter;
    
    // Erase state
    bool FBManualPending;
    bool FBVBErasePending;
    bool FBVBEraseActive;
    
    // Timing
    bool vb_status, hb_status;
    uint64 lastts;
};

// ============================================================================
// VDP1 Main Class
// ============================================================================

class VDP1 {
public:
    VDP1(core::Scheduler& scheduler, sys::SH2Bus& bus);
    ~VDP1();
    
    // Lifecycle
    void Init();
    void Reset(bool hard);
    
    // Main update function
    uint64 Update(uint64 timestamp);
    
    // Register access
    void Write8(uint32 addr, uint8 value);
    void Write16(uint32 addr, uint16 value);
    uint16 Read16(uint32 addr);
    
    // HBlank/VBlank notifications from VDP2
    void SetHBVB(uint64 timestamp, bool new_hb, bool new_vb);
    
    // Get framebuffer for VDP2 composition
    // Returns scanline from the display framebuffer (not the draw framebuffer)
    bool GetLine(int line, uint16* buf, unsigned w, 
                 uint32 rot_x, uint32 rot_y, 
                 uint32 rot_xinc, uint32 rot_yinc);
    
    // State access (for save states)
    VDP1State& GetState() { return m_state; }
    const VDP1State& GetState() const { return m_state; }
    
private:
    // Dependencies
    core::Scheduler& m_scheduler;
    sys::SH2Bus& m_bus;
    
    // State
    VDP1State m_state;
    
    // Command processing
    void ProcessCommands();
    void StartDrawing();
    
    // Register access
    void WriteRegister(unsigned reg_index, uint16 value);
    uint16 ReadRegister(unsigned reg_index) const;
    
    // Command implementations (to be added in separate files)
    sint32 CMD_NormalSprite(const uint16* cmd_data);
    sint32 CMD_ScaledSprite(const uint16* cmd_data);
    sint32 CMD_DistortedSprite(const uint16* cmd_data);
    sint32 CMD_Polygon(const uint16* cmd_data);
    sint32 CMD_Polyline(const uint16* cmd_data);
    sint32 CMD_Line(const uint16* cmd_data);
    sint32 CMD_SetUserClip(const uint16* cmd_data);
    sint32 CMD_SetSystemClip(const uint16* cmd_data);
    sint32 CMD_SetLocalCoord(const uint16* cmd_data);
    
    // Memory helpers
    uint8 ReadVRAM8(uint32 addr) const;
    uint16 ReadVRAM16(uint32 addr) const;
    void WriteVRAM8(uint32 addr, uint8 value);
    void WriteVRAM16(uint32 addr, uint16 value);
    
    uint8 ReadFB8(bool which, uint32 addr) const;
    uint16 ReadFB16(bool which, uint32 addr) const;
    void WriteFB8(bool which, uint32 addr, uint8 value);
    void WriteFB16(bool which, uint32 addr, uint16 value);
};

} // namespace brimir::vdp1

