#pragma once

/**
@file
@brief Frame address utilities.
*/

#include <brimir/core/types.hpp>

namespace brimir::media {

/// @brief Converts a MM:SS:FF timestamp into the corresponding frame address.
/// @param[in] min the minutes
/// @param[in] sec the seconds
/// @param[in] frame the frames
/// @return the frame address corresponding to the given MM:SS:FF
inline uint32 TimestampToFrameAddress(uint32 min, uint32 sec, uint32 frame) {
    return min * 75 * 60 + sec * 75 + frame;
}

/// @brief Converts a MM:SS:FF timestamp into a file offset.
/// @param[in] min the minutes
/// @param[in] sec the seconds
/// @param[in] frame the frames
/// @param[in] sectorSize the sector size
/// @return the offset from the start of the file to the start of the sector corresponding to MM:SS:FF
inline uintmax_t TimestampToFileOffset(uint32 min, uint32 sec, uint32 frame, uint32 sectorSize) {
    return static_cast<uintmax_t>(TimestampToFrameAddress(min, sec, frame)) * sectorSize;
}

} // namespace brimir::media
