#pragma once

/**
 * @file vdp1_render.hpp
 * @brief VDP1 Template-Based Rendering System
 * 
 * Based on Mednafen's VDP1 implementation with adaptations for Brimir.
 * 
 * This file contains the core rendering templates that generate optimized
 * code paths for every combination of rendering modes, eliminating runtime
 * branching for maximum performance.
 * 
 * Copyright (C) 2015-2017 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp1/vdp1_helpers.hpp>
#include <brimir/util/inline.hpp>
#include <algorithm>

namespace brimir::vdp1 {

// Register flag constants (local copies for rendering system)
enum {
    TVMR_8BPP_LOCAL   = 0x01,
    TVMR_ROTATE_LOCAL = 0x02,
    FBCR_DIL_LOCAL    = 0x04,
    FBCR_DIE_LOCAL    = 0x08,
    FBCR_EOS_LOCAL    = 0x10
};

// Forward declarations
extern uint16 VRAM[0x40000];        // 512KB VRAM
extern uint16 FB[2][0x20000];       // Dual 256KB framebuffers
extern bool FBDrawWhich;            // Current draw buffer (0 or 1)

extern sint32 SysClipX, SysClipY;   // System clip coordinates
extern sint32 UserClipX0, UserClipY0, UserClipX1, UserClipY1;  // User clip rectangle
extern sint32 LocalX, LocalY;       // Local coordinate offset

extern uint8 TVMR;                  // TV mode register
extern uint8 FBCR;                  // Framebuffer control register

extern line_data LineSetup;         // Current line drawing setup

// Texture fetch function table (32 entries for all texture modes)
extern uint32 (*const TexFetchTab[0x20])(uint32 x);

// =============================================================================
// PlotPixel Template - Core Pixel Plotting
// =============================================================================

/**
 * @brief Template-based pixel plotter with compile-time optimization
 * 
 * Generates specialized code for every combination of rendering modes.
 * 
 * @tparam die Double interlace enable
 * @tparam bpp8 8-bit color mode (0=off, 1=normal, 2=rotated)
 * @tparam MSBOn MSB on (color calculation mode)
 * @tparam UserClipEn User clipping enable
 * @tparam UserClipMode User clipping mode (0=outside, 1=inside)
 * @tparam MeshEn Mesh pattern enable
 * @tparam HalfFGEn Half-transparency foreground
 * @tparam HalfBGEn Half-transparency background
 * 
 * @param x X coordinate
 * @param y Y coordinate
 * @param pix Pixel color (RGB555)
 * @param transparent Transparency flag
 * @param g Gouraud shading interpolator (nullptr if disabled)
 * @return Cycle count for timing emulation
 */
template<bool die, unsigned bpp8, bool MSBOn, bool UserClipEn, bool UserClipMode, 
         bool MeshEn, bool HalfFGEn, bool HalfBGEn>
static inline sint32 PlotPixel(sint32 x, sint32 y, uint16 pix, bool transparent, 
                               GourauderTheTerrible* g)
{
    static_assert(!MSBOn || (!HalfFGEn && !HalfBGEn), 
                  "Table error; sub-optimal template arguments.");
    
    sint32 ret = 0;
    uint16* fbyptr;
    
    // Select framebuffer line based on double interlace mode
    if constexpr(die)
    {
        fbyptr = &FB[FBDrawWhich][((y >> 1) & 0xFF) << 9];
        transparent |= ((y & 1) != (bool)(FBCR & FBCR_DIL_LOCAL));
    }
    else
    {
        fbyptr = &FB[FBDrawWhich][(y & 0xFF) << 9];
    }
    
    // Mesh pattern: checkerboard transparency
    if constexpr(MeshEn)
        transparent |= (x ^ y) & 1;
    
    // 8-bit color mode
    if constexpr(bpp8)
    {
        if constexpr(MSBOn)
        {
            // Read background pixel for color calculation
            pix = (fbyptr[((x >> 1) & 0x1FF)] | 0x8000) >> (((x & 1) ^ 1) << 3);
            ret += 5;
        }
        else if constexpr(HalfBGEn)
        {
            ret += 5;
        }
        
        if(!transparent)
        {
            if constexpr(bpp8 == 2)  // 8BPP + rotated
            {
                // Rotation mode addressing
                const uint32 addr = (x & 0x1FF) | ((y & 0x100) << 1);
                const uint32 shift = ((x & 1) ^ 1) << 3;
                const uint32 mask = 0xFF << shift;
                fbyptr[addr >> 1] = (fbyptr[addr >> 1] & ~mask) | ((pix << shift) & mask);
            }
            else  // Normal 8BPP
            {
                const uint32 addr = x & 0x3FF;
                const uint32 shift = ((x & 1) ^ 1) << 3;
                const uint32 mask = 0xFF << shift;
                fbyptr[addr >> 1] = (fbyptr[addr >> 1] & ~mask) | ((pix << shift) & mask);
            }
        }
        ret++;
    }
    else  // 16-bit color mode
    {
        uint16* const p = &fbyptr[x & 0x1FF];
        
        if constexpr(MSBOn)
        {
            // Read background pixel, set MSB
            pix = *p | 0x8000;
            ret += 5;
        }
        else
        {
            if constexpr(HalfBGEn)
            {
                // Background half-transparency (color blending)
                uint16 bg_pix = *p;
                ret += 5;
                
                if(bg_pix & 0x8000)
                {
                    if constexpr(HalfFGEn)
                    {
                        // Apply Gouraud if enabled
                        if(g)
                            pix = g->Apply(pix);
                        
                        // Blend foreground and background (average)
                        pix = ((pix + bg_pix) - ((pix ^ bg_pix) & 0x8421)) >> 1;
                    }
                    else
                    {
                        if(g)
                            pix = 0;  // Special case for Gouraud + HalfBG without HalfFG
                        else
                            pix = ((bg_pix & 0x7BDE) >> 1) | (bg_pix & 0x8000);
                    }
                }
                else
                {
                    if constexpr(HalfFGEn)
                    {
                        if(g)
                            pix = g->Apply(pix);
                        else
                            pix = pix;
                    }
                    else
                    {
                        if(g)
                            pix = 0;
                        else
                            pix = bg_pix;
                    }
                }
            }
            else
            {
                // Apply Gouraud shading if enabled
                if(g)
                    pix = g->Apply(pix);
                
                // Foreground half-transparency (darken)
                if constexpr(HalfFGEn)
                    pix = ((pix & 0x7BDE) >> 1) | (pix & 0x8000);
            }
        }
        
        if(!transparent)
            *p = pix;
        
        ret++;
    }
    
    return ret;
}

// =============================================================================
// DrawLine Template - Bresenham Line Drawing
// =============================================================================

/**
 * @brief Template-based line drawer with compile-time optimization
 * 
 * Implements Bresenham's line algorithm with all rendering modes.
 * 
 * @tparam AA Anti-aliasing enable
 * @tparam die Double interlace enable
 * @tparam bpp8 8-bit color mode
 * @tparam MSBOn MSB on
 * @tparam UserClipEn User clipping enable
 * @tparam UserClipMode User clipping mode
 * @tparam MeshEn Mesh pattern enable
 * @tparam ECD End code disable (texture)
 * @tparam SPD Skip pixel disable (transparency)
 * @tparam Textured Texture mapping enable
 * @tparam GouraudEn Gouraud shading enable
 * @tparam HalfFGEn Half-transparency foreground
 * @tparam HalfBGEn Half-transparency background
 * 
 * @return Cycle count for timing emulation
 */
template<bool AA, bool die, unsigned bpp8, bool MSBOn, bool UserClipEn, bool UserClipMode, 
         bool MeshEn, bool ECD, bool SPD, bool Textured, bool GouraudEn, 
         bool HalfFGEn, bool HalfBGEn>
static sint32 DrawLine()
{
    const uint16 color = LineSetup.color;
    line_vertex p0 = LineSetup.p[0];
    line_vertex p1 = LineSetup.p[1];
    sint32 ret = 0;
    
    // Pre-clipping (PCD = Pre-Clipping Disable)
    if(!LineSetup.PCD)
    {
        bool clipped = false;
        bool swapped = false;
        
        ret += 4;
        
        if constexpr(UserClipEn)
        {
            if constexpr(UserClipMode)
            {
                // Mode 1: Clip inside user rectangle (creates hole)
                clipped |= (p0.x < 0) & (p1.x < 0);
                clipped |= (p0.x > SysClipX) & (p1.x > SysClipX);
                clipped |= (p0.y < 0) & (p1.y < 0);
                clipped |= (p0.y > SysClipY) & (p1.y > SysClipY);
                
                swapped = (p0.y == p1.y) & ((p0.x < 0) | (p0.x > SysClipX));
            }
            else
            {
                // Mode 0: Clip outside user rectangle (normal)
                clipped |= (p0.x < UserClipX0) & (p1.x < UserClipX0);
                clipped |= (p0.x > UserClipX1) & (p1.x > UserClipX1);
                clipped |= (p0.y < UserClipY0) & (p1.y < UserClipY0);
                clipped |= (p0.y > UserClipY1) & (p1.y > UserClipY1);
                
                swapped = (p0.y == p1.y) & ((p0.x < UserClipX0) | (p0.x > UserClipX1));
            }
        }
        else
        {
            // System clip only
            clipped |= (p0.x < 0) & (p1.x < 0);
            clipped |= (p0.x > SysClipX) & (p1.x > SysClipX);
            clipped |= (p0.y < 0) & (p1.y < 0);
            clipped |= (p0.y > SysClipY) & (p1.y > SysClipY);
            
            swapped = (p0.y == p1.y) & ((p0.x < 0) | (p0.x > SysClipX));
        }
        
        if(clipped)
            return ret;
        
        if(swapped)
            std::swap(p0, p1);
    }
    
    ret += 8;
    
    // Bresenham line setup
    const sint32 dx = p1.x - p0.x;
    const sint32 dy = p1.y - p0.y;
    const sint32 abs_dx = std::abs(dx);
    const sint32 abs_dy = std::abs(dy);
    const sint32 max_adx_ady = std::max<sint32>(abs_dx, abs_dy);
    sint32 x_inc = (dx >= 0) ? 1 : -1;
    sint32 y_inc = (dy >= 0) ? 1 : -1;
    sint32 x = p0.x;
    sint32 y = p0.y;
    bool drawn_ac = true;  // Drawn all-clipped
    uint32 texel = 0;
    GourauderTheTerrible g;
    VileTex t;
    
    // Setup Gouraud interpolation
    if constexpr(GouraudEn)
        g.Setup(max_adx_ady + 1, p0.g, p1.g);
    
    // Setup texture interpolation
    if constexpr(Textured)
    {
        LineSetup.ec_count = 2;  // Call before tffn()
        
        if(max_adx_ady < std::abs(p1.t - p0.t) && LineSetup.HSS)
        {
            // High-speed shrink mode
            LineSetup.ec_count = 0x7FFFFFFF;
            t.Setup(max_adx_ady + 1, p0.t >> 1, p1.t >> 1, 2, (bool)(FBCR & FBCR_EOS_LOCAL));
        }
        else
        {
            t.Setup(max_adx_ady + 1, p0.t, p1.t);
        }
        
        texel = LineSetup.tffn(t.Current());
    }
    
    // Pixel processing macros
    #define PSTART                                  \
        bool transparent;                           \
        uint16 pix;                                 \
                                                    \
        if constexpr(Textured)                      \
        {                                           \
            while(t.IncPending())                   \
            {                                       \
                sint32 tx = t.DoPendingInc();       \
                texel = LineSetup.tffn(tx);         \
                                                    \
                if constexpr(!ECD)                  \
                {                                   \
                    if(LineSetup.ec_count <= 0)     \
                        return ret;                 \
                }                                   \
            }                                       \
            t.AddError();                           \
                                                    \
            transparent = (SPD && ECD) ? false : (texel >> 31);  \
            pix = texel;                            \
        }                                           \
        else                                        \
        {                                           \
            pix = color;                            \
            transparent = !SPD;                     \
        }
    
    #define PBODY(px, py)                                       \
        {                                                       \
            bool clipped = ((uint32)px > (uint32)SysClipX) |   \
                           ((uint32)py > (uint32)SysClipY);    \
                                                                \
            if constexpr(UserClipEn && !UserClipMode)           \
            {                                                   \
                clipped |= (px < UserClipX0) | (px > UserClipX1) |  \
                           (py < UserClipY0) | (py > UserClipY1);   \
            }                                                   \
                                                                \
            if((clipped ^ drawn_ac) & clipped)                  \
                return ret;                                     \
                                                                \
            drawn_ac &= clipped;                                \
                                                                \
            if constexpr(UserClipEn && UserClipMode)            \
            {                                                   \
                clipped |= (px >= UserClipX0) & (px <= UserClipX1) &  \
                           (py >= UserClipY0) & (py <= UserClipY1);   \
            }                                                   \
                                                                \
            ret += PlotPixel<die, bpp8, MSBOn, UserClipEn, UserClipMode, MeshEn, HalfFGEn, HalfBGEn>  \
                   (px, py, pix, transparent | clipped, (GouraudEn ? &g : nullptr));  \
        }
    
    #define PEND                    \
        {                           \
            if constexpr(GouraudEn) \
                g.Step();           \
        }
    
    // Bresenham line algorithm - steep (|dy| > |dx|)
    if(abs_dy > abs_dx)
    {
        sint32 error_inc = 2 * abs_dx;
        sint32 error_adj = -(2 * abs_dy);
        sint32 error = abs_dy - (2 * abs_dy + (dy >= 0 || AA));
        
        y -= y_inc;
        
        do
        {
            PSTART;
            
            y += y_inc;
            if(error >= 0)
            {
                if constexpr(AA)
                {
                    // Anti-aliasing: draw extra pixel at inflection
                    sint32 aa_x = x, aa_y = y;
                    
                    if(y_inc < 0)
                    {
                        aa_x += (x_inc >> 31);
                        aa_y -= (x_inc >> 31);
                    }
                    else
                    {
                        aa_x -= (~x_inc >> 31);
                        aa_y += (~x_inc >> 31);
                    }
                    
                    PBODY(aa_x, aa_y);
                }
                
                error += error_adj;
                x += x_inc;
            }
            error += error_inc;
            
            PBODY(x, y);
            
            PEND;
        } while(y != p1.y);
    }
    else  // Shallow (|dx| >= |dy|)
    {
        sint32 error_inc = 2 * abs_dy;
        sint32 error_adj = -(2 * abs_dx);
        sint32 error = abs_dx - (2 * abs_dx + (dx >= 0 || AA));
        
        x -= x_inc;
        
        do
        {
            PSTART;
            
            x += x_inc;
            if(error >= 0)
            {
                if constexpr(AA)
                {
                    // Anti-aliasing: draw extra pixel at inflection
                    sint32 aa_x = x, aa_y = y;
                    
                    if(x_inc < 0)
                    {
                        aa_x -= (~y_inc >> 31);
                        aa_y -= (~y_inc >> 31);
                    }
                    else
                    {
                        aa_x += (y_inc >> 31);
                        aa_y += (y_inc >> 31);
                    }
                    
                    PBODY(aa_x, aa_y);
                }
                
                error += error_adj;
                y += y_inc;
            }
            error += error_inc;
            
            PBODY(x, y);
            
            PEND;
        } while(x != p1.x);
    }
    
    #undef PSTART
    #undef PBODY
    #undef PEND
    
    return ret;
}

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * @brief Check for undefined/illegal clipping windows
 */
static inline void CheckUndefClipping()
{
    if(SysClipX < UserClipX1 || SysClipY < UserClipY1 || 
       UserClipX0 > UserClipX1 || UserClipY0 > UserClipY1)
    {
        // Log warning about illegal clipping windows
    }
}

// Public API
sint32 ExecuteDrawLine();

} // namespace brimir::vdp1

