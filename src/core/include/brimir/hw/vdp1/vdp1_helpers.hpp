#pragma once

/**
 * @file vdp1_helpers.hpp
 * @brief VDP1 Helper Structures for Drawing
 * 
 * Based on Mednafen's VDP1 implementation with adaptations for Brimir.
 * These structures provide efficient Bresenham-style interpolation for
 * Gouraud shading, texture coordinates, and edge stepping.
 * 
 * Copyright (C) 2015-2017 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/core/types.hpp>
#include <brimir/util/inline.hpp>

#include <algorithm>
#include <cstdlib>

namespace brimir::vdp1 {

// =============================================================================
// Forward Declarations / Extern Variables
// =============================================================================

// Lookup tables (defined in vdp1_helpers.cpp) - declared early so they can be used in inline functions
extern uint8 gouraud_lut[0x40];      // Gouraud shading lookup table (64 entries)
extern uint8 spr_w_shift_tab[8];     // Sprite width shift table

/**
 * @brief Initialize Gouraud shading lookup table
 * Must be called before any VDP1 rendering.
 */
void InitGouraudLUT();

// =============================================================================
// GourauderTheTerrible - Gouraud Shading Interpolator
// =============================================================================

/**
 * @brief Bresenham-style Gouraud shading interpolator
 * 
 * Interpolates RGB color values across a line or edge using Bresenham's
 * algorithm for smooth color gradients.
 */
struct GourauderTheTerrible
{
    /**
     * @brief Setup interpolation from start to end color over given length
     * @param length Number of steps for interpolation
     * @param gstart Starting color (RGB555 format, 0x7FFF mask)
     * @param gend Ending color (RGB555 format, 0x7FFF mask)
     */
    void Setup(const unsigned length, const uint16 gstart, const uint16 gend)
    {
        g = gstart & 0x7FFF;
        intinc = 0;
        
        // Interpolate each color component (R, G, B) separately
        for(unsigned cc = 0; cc < 3; cc++)
        {
            const sint32 dg = ((gend >> (cc * 5)) & 0x1F) - ((gstart >> (cc * 5)) & 0x1F);
            const unsigned abs_dg = std::abs(dg);
            
            ginc[cc] = (uint32)((dg >= 0) ? 1 : -1) << (cc * 5);
            
            if(length <= abs_dg)
            {
                error_inc[cc] = (abs_dg + 1) * 2;
                error_adj[cc] = (length * 2);
                error[cc] = abs_dg + 1 - (length * 2 + ((dg < 0) ? 1 : 0));
                
                while(error[cc] >= 0) { g += ginc[cc]; error[cc] -= error_adj[cc]; }
                while(error_inc[cc] >= error_adj[cc]) { intinc += ginc[cc]; error_inc[cc] -= error_adj[cc]; }
            }
            else
            {
                error_inc[cc] = abs_dg * 2;
                error_adj[cc] = ((length - 1) * 2);
                error[cc] = length - (length * 2 - ((dg < 0) ? 1 : 0));
                if(error[cc] >= 0) { g += ginc[cc]; error[cc] -= error_adj[cc]; }
                if(error_inc[cc] >= error_adj[cc]) { intinc += ginc[cc]; error_inc[cc] -= error_adj[cc]; }
            }
            error[cc] = ~error[cc];
        }
    }
    
    /**
     * @brief Get current interpolated color value
     * @return Current color (RGB555 format)
     */
    FORCE_INLINE uint32 Current() const
    {
        return g;
    }
    
    /**
     * @brief Apply Gouraud shading to a pixel
     * @param pix Input pixel color
     * @return Pixel with Gouraud shading applied
     */
    FORCE_INLINE uint16 Apply(uint16 pix) const
    {
        uint16 ret = pix & 0x8000;  // Preserve MSB
        
        ret |= gouraud_lut[((pix & (0x1F <<  0)) + (g & (0x1F <<  0))) >>  0] <<  0;  // Red
        ret |= gouraud_lut[((pix & (0x1F <<  5)) + (g & (0x1F <<  5))) >>  5] <<  5;  // Green
        ret |= gouraud_lut[((pix & (0x1F << 10)) + (g & (0x1F << 10))) >> 10] << 10;  // Blue
        
        return ret;
    }
    
    /**
     * @brief Step to next interpolated value
     */
    FORCE_INLINE void Step()
    {
        g += intinc;
        
        for(unsigned cc = 0; cc < 3; cc++)
        {
            error[cc] -= error_inc[cc];
            {
                const uint32 mask = (sint32)error[cc] >> 31;
                g += ginc[cc] & mask;
                error[cc] += error_adj[cc] & mask;
            }
        }
    }
    
    uint32 g;              // Current interpolated color
    uint32 intinc;         // Integer increment per step
    sint32 ginc[3];        // Increment for each component (R, G, B)
    sint32 error[3];       // Error accumulator for each component
    sint32 error_inc[3];   // Error increment for each component
    sint32 error_adj[3];   // Error adjustment for each component
};

// =============================================================================
// VileTex - Texture Coordinate Interpolator
// =============================================================================

/**
 * @brief Bresenham-style texture coordinate interpolator
 * 
 * Interpolates texture coordinates across a line or edge using Bresenham's
 * algorithm for accurate texture mapping.
 */
struct VileTex
{
    /**
     * @brief Setup interpolation from start to end coordinate over given length
     * @param length Number of steps for interpolation
     * @param tstart Starting texture coordinate
     * @param tend Ending texture coordinate
     * @param sf Scale factor (default 1)
     * @param tfudge Fudge factor for sub-pixel precision (default 0)
     * @return false (always)
     */
    FORCE_INLINE bool Setup(const unsigned length, const sint32 tstart, const sint32 tend, 
                            const sint32 sf = 1, const sint32 tfudge = 0)
    {
        sint32 dt = tend - tstart;
        unsigned abs_dt = std::abs(dt);
        
        t = (tstart * sf) | tfudge;
        
        tinc = (dt >= 0) ? sf : -sf;
        
        if(length <= abs_dt)
        {
            error_inc = (abs_dt + 1) * 2;
            error_adj = (length * 2);
            error = abs_dt + 1 - (length * 2 + ((dt < 0) ? 1 : 0));
        }
        else
        {
            error_inc = abs_dt * 2;
            error_adj = ((length - 1) * 2);
            error = length - (length * 2 - ((dt < 0) ? 1 : 0));
        }
        
        return false;
    }
    
    /**
     * @brief Check if increment is pending
     * @return true if error >= 0
     */
    FORCE_INLINE bool IncPending() const { return error >= 0; }
    
    /**
     * @brief Perform pending increment
     * @return New texture coordinate after increment
     */
    FORCE_INLINE sint32 DoPendingInc() { t += tinc; error -= error_adj; return t; }
    
    /**
     * @brief Add error increment
     */
    FORCE_INLINE void AddError() { error += error_inc; }
    
    /**
     * @brief Pre-step: perform all pending increments and add error
     * @return Current texture coordinate after pre-stepping
     */
    FORCE_INLINE sint32 PreStep()
    {
        while(error >= 0)
        {
            t += tinc;
            error -= error_adj;
        }
        error += error_inc;
        
        return t;
    }
    
    /**
     * @brief Get current texture coordinate
     * @return Current coordinate
     */
    FORCE_INLINE sint32 Current() const
    {
        return t;
    }
    
    sint32 t;           // Current texture coordinate
    sint32 tinc;        // Increment per step
    sint32 error;       // Error accumulator
    sint32 error_inc;   // Error increment
    sint32 error_adj;   // Error adjustment
};

// =============================================================================
// EdgeStepper - Polygon Edge Stepper
// =============================================================================

/**
 * @brief Line vertex structure
 */
struct line_vertex
{
    sint32 x, y;    // Screen coordinates
    uint16 g;       // Gouraud color
    sint32 t;       // Texture coordinate
};

/**
 * @brief Edge stepper for polygon rasterization
 * 
 * @tparam gourauden Enable Gouraud shading interpolation
 */
template<bool gourauden>
struct EdgeStepper
{
    /**
     * @brief Setup edge stepping from p0 to p1
     * @param p0 Start vertex
     * @param p1 End vertex
     * @param dmax Maximum distance
     */
    FORCE_INLINE void Setup(const line_vertex& p0, const line_vertex& p1, const sint32 dmax)
    {
        sint32 dx = p1.x - p0.x;
        sint32 dy = p1.y - p0.y;
        sint32 abs_dx = std::abs(dx);
        sint32 abs_dy = std::abs(dy);
        sint32 max_adxdy = std::max<sint32>(abs_dx, abs_dy);
        
        x = p0.x;
        x_inc = (dx >= 0) ? 1 : -1;
        x_error = ~(max_adxdy - (2 * max_adxdy + (dy >= 0)));
        x_error_inc = 2 * abs_dx;
        x_error_adj = 2 * max_adxdy;
        
        y = p0.y;
        y_inc = (dy >= 0) ? 1 : -1;
        y_error = ~(max_adxdy - (2 * max_adxdy + (dx >= 0)));
        y_error_inc = 2 * abs_dy;
        y_error_adj = 2 * max_adxdy;
        
        d_error = -dmax;
        d_error_inc = 2 * max_adxdy;
        d_error_adj = 2 * dmax;
        
        if constexpr(gourauden)
            g.Setup(max_adxdy + 1, p0.g, p1.g);
    }
    
    /**
     * @brief Get current vertex
     * @param p Output vertex
     */
    FORCE_INLINE void GetVertex(line_vertex* p)
    {
        p->x = x;
        p->y = y;
        
        if constexpr(gourauden)
            p->g = g.Current();
    }
    
    /**
     * @brief Step to next edge position
     */
    FORCE_INLINE void Step()
    {
        uint32 mask;
        
        d_error += d_error_inc;
        if(d_error >= 0)
        {
            d_error -= d_error_adj;
            
            x_error -= x_error_inc;
            mask = (sint32)x_error >> 31;
            x += x_inc & mask;
            x_error += x_error_adj & mask;
            
            y_error -= y_error_inc;
            mask = (sint32)y_error >> 31;
            y += y_inc & mask;
            y_error += y_error_adj & mask;
            
            if constexpr(gourauden)
                g.Step();
        }
    }
    
    sint32 d_error, d_error_inc, d_error_adj;  // Distance error tracking
    
    sint32 x, x_inc;                           // X position and increment
    sint32 x_error, x_error_inc, x_error_adj;  // X error tracking
    
    sint32 y, y_inc;                           // Y position and increment
    sint32 y_error, y_error_inc, y_error_adj;  // Y error tracking
    
    GourauderTheTerrible g;                     // Gouraud interpolator (if enabled)
};

// =============================================================================
// Line Drawing Data
// =============================================================================

/**
 * @brief Line drawing setup data
 */
struct line_data
{
    line_vertex p[2];       // Start and end vertices
    bool PCD;               // Pre-clipping disable
    bool HSS;               // High-speed shrink
    uint16 color;           // Line color
    sint32 ec_count;        // End code count
    uint32 (*tffn)(uint32); // Texture fetch function pointer
    uint16 CLUT[0x10];      // Color lookup table (16 entries)
    uint32 cb_or;           // Color bank OR value
    uint32 tex_base;        // Texture base address
};

} // namespace brimir::vdp1




