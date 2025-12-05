/**
 * @file vdp2.hpp
 * @brief VDP2 Main Interface and State
 * 
 * Based on Mednafen's VDP2 implementation with adaptations for Brimir.
 * VDP2 handles background layers, compositing, and final display output.
 * 
 * Copyright (C) 2015-2019 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#pragma once

#include <brimir/core/types.hpp>
#include <array>

namespace brimir::vdp2 {

// =============================================================================
// Constants
// =============================================================================

constexpr uint32 VRAM_SIZE = 0x80000;   // 512KB VRAM
constexpr uint32 CRAM_SIZE = 0x1000;    // 4KB Color RAM
constexpr uint32 REG_SIZE = 0x200;      // Register space

constexpr uint32 MAX_WIDTH = 704;       // Maximum horizontal resolution
constexpr uint32 MAX_HEIGHT = 512;      // Maximum vertical resolution (interlace)

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Horizontal resolution modes
 */
enum class HResMode : uint8 {
    Normal = 0,         // 320 pixels
    HiRes = 1,          // 640 pixels
    Exclusive = 2,      // 352 pixels (exclusive)
    ExclusiveHi = 3     // 704 pixels (exclusive hi-res)
};

/**
 * @brief Vertical resolution modes
 */
enum class VResMode : uint8 {
    Lines224 = 0,
    Lines240 = 1,
    Lines256 = 2
};

/**
 * @brief Interlace modes
 */
enum class InterlaceMode : uint8 {
    None = 0,
    Illegal = 1,
    Single = 2,
    Double = 3
};

/**
 * @brief Color RAM modes
 */
enum class CRAMMode : uint8 {
    RGB555_1024 = 0,    // 1024 colors, RGB555
    RGB555_2048 = 1,    // 2048 colors, RGB555
    RGB888_1024 = 2,    // 1024 colors, RGB888
    Illegal = 3
};

/**
 * @brief Background layer identifiers
 */
enum class Layer : uint8 {
    NBG0 = 0,           // Normal background 0
    NBG1 = 1,           // Normal background 1
    NBG2 = 2,           // Normal background 2
    NBG3 = 3,           // Normal background 3
    RBG0 = 4,           // Rotation background 0
    RBG1 = 5,           // Rotation background 1
    Sprite = 6,         // Sprite layer (from VDP1)
    Back = 7            // Back screen
};

/**
 * @brief Window identifiers
 */
enum class WindowId : uint8 {
    Window0 = 0,
    Window1 = 1
};

// =============================================================================
// Structures
// =============================================================================

/**
 * @brief VDP2 register state
 */
struct VDP2Regs {
    // TV Screen Mode (TVMD)
    bool displayOn;
    bool borderMode;
    HResMode hRes;
    VResMode vRes;
    InterlaceMode interlace;
    
    // External Signal Enable (EXTEN)
    bool exLatchEnable;
    bool exSyncEnable;
    bool exBGEnable;
    bool dispAreaSelect;
    
    // RAM Control (RAMCTL)
    CRAMMode cramMode;
    uint8 vramMode;
    bool coeffTableEnable;
    
    // Background ON (BGON)
    uint16 bgon;
    
    // Character control
    uint16 chctla;  // NBG0/1
    uint16 chctlb;  // NBG2/3/RBG0
    
    // Pattern name control
    uint16 pncn[4];     // NBG0-3
    uint16 pncr;        // RBG0
    
    // Plane size
    uint16 plsz;
    
    // Map offset
    uint16 mpofn;   // NBG
    uint16 mpofr;   // RBG
    
    // Map plane addresses (MPABN0-3)
    uint16 mpabn[4];
    
    // Bitmap palette (BMPNA)
    uint16 bmpna;
    
    // Scroll values (integer parts)
    sint16 scrollX[4];
    sint16 scrollY[4];
    
    // Scroll values (decimal parts, NBG0/1 only)
    uint16 scrollXD[2];
    uint16 scrollYD[2];
    
    // Zoom values (NBG0/1 only)
    uint16 zoomX[2];
    uint16 zoomY[2];
    
    // Window control
    uint8 winControl[8];
    
    // Priority
    uint8 nbgPrio[4];
    uint8 rbgPrio;
    uint8 spritePrio[8];
    
    // Color calculation
    uint16 ccctl;
    uint8 nbgCCRatio[4];
    uint8 rbgCCRatio;
    uint8 spriteCCRatio[8];
    
    // Color offset
    bool colorOffsetEnable[8];
    sint16 colorOffsetR[2];
    sint16 colorOffsetG[2];
    sint16 colorOffsetB[2];
};

/**
 * @brief Rotation parameters (from VRAM table)
 */
struct RotationParams {
    // Screen start coordinates (1.12.10 fixed point)
    sint32 Xst, Yst, Zst;
    
    // Delta screen coordinates (1.2.10)
    sint32 DXst, DYst;
    sint32 DX, DY;
    
    // Rotation matrix (1.3.10)
    sint32 matrix[6];
    
    // View point (1.13.0)
    sint32 Px, Py, Pz;
    
    // Center coordinates (1.13.0)
    sint32 Cx, Cy, Cz;
    
    // Amount of movement (1.13.10)
    sint32 Mx, My;
    
    // Scaling (1.7.16)
    sint32 kx, ky;
    
    // Coefficient table address
    uint32 KAst;
    uint32 DKAst, DKAx;
    
    // Accumulators
    uint32 XstAccum, YstAccum;
    uint32 KAstAccum;
};

/**
 * @brief Window state
 */
struct WindowState {
    uint16 xStart, xEnd;
    uint16 yStart, yEnd;
    bool yEndMet;
    bool yIn;
    uint32 lineWinAddr;
    bool lineWinEnable;
};

// =============================================================================
// Global State (extern declarations)
// =============================================================================

// Memory
extern uint16 VRAM[VRAM_SIZE / 2];
extern uint16 CRAM[CRAM_SIZE / 2];

// Registers
extern VDP2Regs Regs;

// Rotation parameters
extern RotationParams RotParams[2];

// Window state
extern WindowState Windows[2];

// Timing
extern sint32 VCounter;
extern sint32 HCounter;
extern bool VBlankOut;
extern bool HBlankOut;
extern bool PAL;

// Display
extern uint32 HRes;     // Current horizontal resolution
extern uint32 VRes;     // Current vertical resolution (doubled for interlace)
extern uint32 VResBase; // Base vertical resolution (before interlace doubling)
extern bool CurrentField;  // Current interlace field (false=even, true=odd)
extern bool InterlaceDouble; // True if double-density interlace mode

// VDP1 Framebuffer (for sprite compositing)
extern const uint16* VDP1Framebuffer;
extern uint32 VDP1FBWidth;
extern uint32 VDP1FBHeight;

// =============================================================================
// Core Functions
// =============================================================================

/**
 * @brief Initialize VDP2
 * @param isPAL True for PAL mode, false for NTSC
 */
void Init(bool isPAL);

/**
 * @brief Reset VDP2 state
 * @param poweringUp True for power-on reset
 */
void Reset(bool poweringUp);

/**
 * @brief Shutdown VDP2
 */
void Kill();

/**
 * @brief Update VDP2 state
 * @param timestamp Current CPU timestamp
 * @return Next event timestamp
 */
sint64 Update(sint64 timestamp);

/**
 * @brief Start a new frame
 * @param clock28m True for 28MHz clock mode
 */
void StartFrame(bool clock28m);

/**
 * @brief End current frame
 */
void EndFrame();

/**
 * @brief Set VDP1 framebuffer for sprite compositing
 * @param data Framebuffer data (RGB555)
 * @param width Framebuffer width
 * @param height Framebuffer height
 */
void SetVDP1Framebuffer(const uint16* data, uint32 width, uint32 height);

/**
 * @brief Clear VDP1 framebuffer reference
 */
void ClearVDP1Framebuffer();

/**
 * @brief Set layer enable mask
 * @param mask Bitmask of enabled layers
 */
void SetLayerEnableMask(uint64 mask);

} // namespace brimir::vdp2


