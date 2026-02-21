#pragma once

/**
 * @file vdp1_commands.hpp
 * @brief VDP1 Drawing Command Declarations
 * 
 * Based on Mednafen's VDP1 implementation with adaptations for Brimir.
 * 
 * Copyright (C) 2015-2017 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/core/types.hpp>

namespace brimir::vdp1 {

// =============================================================================
// Drawing Command Functions
// =============================================================================

/**
 * @brief Draw a single line
 * @param cmd_data Command data array (16 words)
 * @return Cycle count
 */
sint32 CMD_Line(const uint16* cmd_data);

/**
 * @brief Draw 4 connected lines (polyline)
 * @param cmd_data Command data array (16 words)
 * @return Cycle count
 */
sint32 CMD_Polyline(const uint16* cmd_data);

/**
 * @brief Draw a filled polygon
 * @param cmd_data Command data array (16 words)
 * @return Cycle count
 */
sint32 CMD_Polygon(const uint16* cmd_data);

/**
 * @brief Draw a normal sprite (no scaling)
 * @param cmd_data Command data array (16 words)
 * @return Cycle count
 */
sint32 CMD_NormalSprite(const uint16* cmd_data);

/**
 * @brief Draw a scaled sprite
 * @param cmd_data Command data array (16 words)
 * @return Cycle count
 */
sint32 CMD_ScaledSprite(const uint16* cmd_data);

/**
 * @brief Draw a distorted sprite (affine transform)
 * @param cmd_data Command data array (16 words)
 * @return Cycle count
 */
sint32 CMD_DistortedSprite(const uint16* cmd_data);

/**
 * @brief Set system clipping rectangle
 * @param cmd_data Command data array (16 words)
 * @return Cycle count
 */
sint32 CMD_SetSystemClip(const uint16* cmd_data);

/**
 * @brief Set user clipping rectangle
 * @param cmd_data Command data array (16 words)
 * @return Cycle count
 */
sint32 CMD_SetUserClip(const uint16* cmd_data);

/**
 * @brief Set local coordinate offset
 * @param cmd_data Command data array (16 words)
 * @return Cycle count
 */
sint32 CMD_SetLocalCoord(const uint16* cmd_data);

} // namespace brimir::vdp1










