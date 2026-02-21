/**
 * @file vdp1_render.cpp
 * @brief VDP1 Rendering System Implementation
 * 
 * Based on Mednafen's VDP1 implementation with adaptations for Brimir.
 * 
 * This file instantiates the rendering templates and creates function
 * pointer tables for dynamic dispatch.
 * 
 * Copyright (C) 2015-2017 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp1/vdp1_render.hpp>
#include <brimir/util/dev_log.hpp>

namespace brimir::vdp1 {

// Dev log groups (same as vdp1.cpp)
namespace grp {
    struct base {
        static constexpr bool enabled = true;
        static constexpr devlog::Level level = devlog::level::debug;
    };

    struct vdp1 : public base {
        static constexpr std::string_view name = "VDP1";
    };

    struct vdp1_cmd : public vdp1 {
        static constexpr std::string_view name = "VDP1-Cmd";
    };
}

// =============================================================================
// Global State
// =============================================================================

uint16 VRAM[0x40000];          // 512KB VRAM (256K words)
uint16 FB[2][0x20000];         // Dual 256KB framebuffers (128K words each)
bool FBDrawWhich = false;      // Current draw buffer (0 or 1)

sint32 SysClipX = 511;         // System clip X (default max)
sint32 SysClipY = 255;         // System clip Y (default max)
sint32 UserClipX0 = 0;         // User clip left
sint32 UserClipY0 = 0;         // User clip top
sint32 UserClipX1 = 511;       // User clip right
sint32 UserClipY1 = 255;       // User clip bottom
sint32 LocalX = 0;             // Local coordinate offset X
sint32 LocalY = 0;             // Local coordinate offset Y

uint8 TVMR = 0;                // TV mode register
uint8 FBCR = 0;                // Framebuffer control register

line_data LineSetup;           // Current line drawing setup

// =============================================================================
// Texture Fetch Function Table
// =============================================================================


// Texture fetch table is now defined in vdp1_texture.cpp

// =============================================================================
// DrawLine Function Table Generation
// =============================================================================

/**
 * @brief DrawLine function pointer type
 */
using DrawLineFn = sint32 (*)();

/**
 * @brief Template instantiation macro for DrawLine
 * 
 * This macro generates all valid combinations of template parameters.
 * Invalid combinations (e.g., MSBOn with HalfFG/BG) are filtered out.
 */
#define INSTANTIATE_DRAWLINE(aa, die, bpp8, msbon, ucen, ucmode, mesh, ecd, spd, tex, gour, hfg, hbg) \
    DrawLine<aa, die, bpp8, msbon, ucen, ucmode, mesh, ecd, spd, tex, gour, hfg, hbg>

// =============================================================================
// Simplified Function Table (Common Cases)
// =============================================================================

/**
 * @brief Simplified DrawLine dispatcher
 * 
 * For Phase 1, we implement the most common rendering modes:
 * - Non-textured solid color lines
 * - Non-textured Gouraud shaded lines
 * - Textured lines (basic)
 * 
 * Full template instantiation will be added in Phase 2.
 */
struct DrawLineParams
{
    bool aa;            // Anti-aliasing
    bool die;           // Double interlace
    unsigned bpp8;      // 8-bit mode (0/1/2)
    bool msbon;         // MSB on
    bool ucen;          // User clip enable
    bool ucmode;        // User clip mode
    bool mesh;          // Mesh enable
    bool ecd;           // End code disable
    bool spd;           // Skip pixel disable
    bool textured;      // Textured
    bool gouraud;       // Gouraud
    bool hfg;           // Half foreground
    bool hbg;           // Half background
};

/**
 * @brief Get DrawLine function for given parameters
 * 
 * This is a simplified dispatch that will be expanded to a full
 * function pointer table in Phase 2.
 */
static sint32 DispatchDrawLine(const DrawLineParams& params)
{
    // Simplified dispatch - only handle common cases for now
    
    // Case 1: Simple solid color line (no effects)
    if(!params.textured && !params.gouraud && !params.mesh && 
       !params.hfg && !params.hbg && !params.msbon)
    {
        if(!params.aa && !params.die && params.bpp8 == 0 && 
           !params.ucen && params.spd)
        {
            return INSTANTIATE_DRAWLINE(false, false, 0, false, false, false, 
                                       false, false, true, false, false, false, false)();
        }
    }
    
    // Case 2: Gouraud shaded line (no texture)
    if(!params.textured && params.gouraud && !params.mesh && 
       !params.hfg && !params.hbg && !params.msbon)
    {
        if(!params.aa && !params.die && params.bpp8 == 0 && 
           !params.ucen && params.spd)
        {
            return INSTANTIATE_DRAWLINE(false, false, 0, false, false, false, 
                                       false, false, true, false, true, false, false)();
        }
    }
    
    // Default: fallback to simplest case
    devlog::warn<grp::vdp1_cmd>("DrawLine: Unimplemented parameter combination, using fallback");
    return INSTANTIATE_DRAWLINE(false, false, 0, false, false, false, 
                               false, false, true, false, false, false, false)();
}

// =============================================================================
// Public API
// =============================================================================

} // namespace brimir::vdp1

// Export for command functions
namespace brimir::vdp1 {

/**
 * @brief Execute line drawing with current LineSetup
 * 
 * @return Cycle count
 */
sint32 ExecuteDrawLine()
{
    // Build parameters from current state
    DrawLineParams params;
    params.aa = false;  // TODO: Extract from command
    params.die = (FBCR & FBCR_DIE_LOCAL) != 0;
    params.bpp8 = (TVMR & TVMR_8BPP_LOCAL) ? ((TVMR & TVMR_ROTATE_LOCAL) ? 2 : 1) : 0;
    params.msbon = false;  // TODO: Extract from command
    params.ucen = false;   // TODO: Extract from command
    params.ucmode = false; // TODO: Extract from command
    params.mesh = false;   // TODO: Extract from command
    params.ecd = false;    // TODO: Extract from command
    params.spd = true;     // TODO: Extract from command
    params.textured = false;  // TODO: Extract from command
    params.gouraud = false;   // TODO: Extract from command
    params.hfg = false;    // TODO: Extract from command
    params.hbg = false;    // TODO: Extract from command
    
    return DispatchDrawLine(params);
}

} // namespace brimir::vdp1

