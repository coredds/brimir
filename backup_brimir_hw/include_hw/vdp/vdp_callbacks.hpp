#pragma once

/**
@file
@brief VDP callbacks.
*/

#include <brimir/core/types.hpp>

#include <brimir/util/callback.hpp>

namespace brimir::vdp {

// Invoked when the VDP1 finishes drawing a frame.
using CBVDP1DrawFinished = util::OptionalCallback<void()>;

// Invoked when the VDP1 swaps framebuffers.
using CBVDP1FramebufferSwap = util::OptionalCallback<void()>;

// Invoked when the VDP2 renderer finishes rendering a frame.
// Framebuffer data is in little-endian XRGB8888 format.
using CBFrameComplete = util::OptionalCallback<void(uint32 *fb, uint32 width, uint32 height)>;

} // namespace brimir::vdp
