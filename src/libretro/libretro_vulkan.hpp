// Brimir - libretro Vulkan hardware context (BRIMIR_VULKAN builds only)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// The frontend side of the GPU renderer: asks the frontend for a Vulkan context at content load (SET_HW_RENDER
// plus the context negotiation interface v2), builds the GPU frame output on the device the frontend hands over in
// context_reset and presents each frame through retro_hw_render_interface_vulkan::set_image.
// See docs/ymir-hw-renderer-notes/stage1-libretro-vulkan.md for the contract.

#pragma once

#include "libretro.h"

#include <cstdint>
#include <memory>
#include <string>

namespace brimir::libretro {

struct VulkanFrameSettings {
    uint32_t cropH = 0;
    uint32_t cropV = 0;
    uint32_t rotation = 0;
};

// Calls SET_HW_RENDER (RETRO_HW_CONTEXT_VULKAN, Vulkan 1.2) and registers the negotiation interface. Call from
// retro_load_game only. Returns false and sets reason when the frontend refuses; the core then stays on software.
// log may be null.
bool RequestVulkanContext(retro_environment_t environment, retro_hw_context_reset_t contextReset,
                          retro_hw_context_reset_t contextDestroy, retro_log_printf_t log, std::string &reason);

// The GPU frame output on the frontend's device: the FramePresenter with one output image per sync slot, plus
// a command buffer and fence per slot. Every call must come from the libretro thread (retro_run, context_reset or
// context_destroy). 1B replaces "present the software frame" with "present the Vulkan VDP renderer's output"
// behind this class.
class VulkanOutput {
public:
    VulkanOutput();
    ~VulkanOutput();
    VulkanOutput(const VulkanOutput &) = delete;
    VulkanOutput &operator=(const VulkanOutput &) = delete;

    // context_reset: fetches the interface and builds everything on its device. Tears down a previous context on
    // the same device first. Returns false (Live() stays false) and sets reason when the device is unusable.
    bool ContextReset(retro_environment_t environment, std::string &reason);
    // context_destroy: waits for the device to be idle and destroys everything. Safe to call when not live.
    void ContextDestroy();
    // Forgets the context without any Vulkan call (its device may already be gone). Leaks the objects.
    void Abandon();

    bool Live() const;

    // Presents a native frame (XBGR8888, tightly packed) with the output transforms: waits for the frontend's
    // sync slot, records the upload and conversion, submits (queue lock held only around vkQueueSubmit) and hands
    // the slot's image over with set_image. Sets outW/outH to the size to report to video_cb. Returns false and
    // sets error when nothing was handed over.
    bool Present(const uint32_t *xbgr, uint32_t w, uint32_t h, const VulkanFrameSettings &settings, unsigned &outW,
                 unsigned &outH, std::string &error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace brimir::libretro
