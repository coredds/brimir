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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace brimir::gpu {
class VulkanVDPRenderer;
} // namespace brimir::gpu

namespace brimir::libretro {

struct VulkanFrameSettings {
    uint32_t cropH = 0;
    uint32_t cropV = 0;
    uint32_t rotation = 0;
};

enum class VulkanRequest {
    Accepted,        // SET_HW_RENDER accepted: the frontend calls context_reset after retro_load_game
    NotVulkanDriver, // GET_PREFERRED_HW_RENDER reports another driver: SET_HW_RENDER is not even tried
    Refused,         // SET_HW_RENDER refused
};

// Unless GET_PREFERRED_HW_RENDER reports a driver other than Vulkan, calls SET_HW_RENDER (RETRO_HW_CONTEXT_VULKAN,
// Vulkan 1.2) and registers the negotiation interface. Call from retro_load_game only. Anything but Accepted sets
// reason; the core then stays on software. log may be null.
VulkanRequest RequestVulkanContext(retro_environment_t environment, retro_hw_context_reset_t contextReset,
                                   retro_hw_context_reset_t contextDestroy, retro_log_printf_t log,
                                   std::string &reason);

enum class VulkanOutputMode {
    None,     // no context, or no usable output: software frames
    Compute,  // FramePresenter: the native frame converted, cropped and rotated by a compute pass
    CopyOnly, // the bridge's converted frame copied into the image (Vulkan 1.0 transfers; no compute, no 1.2)
};

// The GPU frame output on the frontend's device: the FramePresenter with one output image per sync slot, plus
// a command buffer and fence per slot. Every call must come from the libretro thread (retro_run, context_reset or
// context_destroy).
//
// In compute mode it can carry the Vulkan VDP renderer (VDP2 on the GPU): the renderer gets one frame context per
// sync slot (guarded by the slot fences) plus kHiddenContexts contexts for frames with video disabled, each with a
// command buffer and fence of this class, used round-robin. The output owns the device order: the renderer is
// detached after the device is idle and before the slots and the device go.
class VulkanOutput {
public:
    static constexpr uint32_t kHiddenContexts = 2;

    VulkanOutput();
    ~VulkanOutput();
    VulkanOutput(const VulkanOutput &) = delete;
    VulkanOutput &operator=(const VulkanOutput &) = delete;

    // context_reset: fetches the interface and builds everything on its device. Tears down a previous context on
    // the same device first. Uses the compute output when the device allows it, else the copy-only output (reason
    // then says why). Returns false (Live() stays false) and sets reason when there is no usable device at all.
    bool ContextReset(retro_environment_t environment, std::string &reason);
    // context_destroy: waits for the device to be idle, detaches the renderer and destroys everything. Safe to call
    // when not live. The renderer stays installed; it is attached again after the next context_reset.
    void ContextDestroy();
    // Forgets the context (and the renderer's device objects) without any Vulkan call (its device may already be
    // gone). Leaks the objects.
    void Abandon();

    bool Live() const;
    VulkanOutputMode Mode() const;

    // Compute mode: presents a native frame (XBGR8888, tightly packed) with the output transforms: waits for the
    // frontend's sync slot, records the upload and conversion, submits (queue lock held only around vkQueueSubmit)
    // and hands the slot's image over with set_image. Sets outW/outH to the size to report to video_cb. Returns
    // false and sets error when nothing was handed over; Mode() may have changed then (re-creating the slots for a
    // larger sync mask failed: copy-only, or None with the image withdrawn).
    bool Present(const uint32_t *xbgr, uint32_t w, uint32_t h, const VulkanFrameSettings &settings, unsigned &outW,
                 unsigned &outH, std::string &error);
    // Copy-only mode: the same with the bridge's final XRGB8888 frame (already converted, cropped and rotated).
    bool PresentCopy(const void *xrgb, uint32_t w, uint32_t h, size_t pitch, std::string &error);

    // Compute mode: attaches the Vulkan VDP renderer to the device (sync slots + kHiddenContexts frame contexts),
    // detaching another one first. Call between frames. Returns false (renderer not attached) with error on failure.
    bool AttachRenderer(gpu::VulkanVDPRenderer &renderer, std::string &error);
    // Waits for this output's own submissions and detaches the renderer. No-op without one. Call between frames.
    // Returns false (and still detaches) when a wait failed, with error set.
    bool DetachRenderer(std::string &error);
    bool RendererAttached() const;

    // Renderer path: presents the renderer's frame of this retro_run (recorded into the slot's command buffer), or
    // re-presents the composite (no VDP2 frame ended in this run) as w x h with the output transforms, in one submit,
    // and hands the slot's image over. Returns false when nothing was handed over; error is empty when there was just
    // nothing to show yet (a fresh composite), else says why (Mode() may have changed as with Present).
    bool PresentRenderer(uint32_t w, uint32_t h, const VulkanFrameSettings &settings, unsigned &outW, unsigned &outH,
                         std::string &error);
    // Renderer path with video disabled: records the renderer's frame into the next hidden context and submits it
    // (no presenter, no set_image), at the renderer's current internal resolution. True when there was nothing to
    // record.
    bool SubmitHidden(std::string &error);

    // Internal resolution n (1-4, brimir_internal_resolution) for the Vulkan VDP renderer. Kept across contexts: each
    // attach allocates it directly (the renderer falls back, logged, when it cannot). A change applies at the next
    // shown frame that has a VDP2 frame to record (PresentRenderer), before it is recorded: wait for this output's slot
    // and
    // hidden-context fences (and the frontend's queue when it may still sample the output images), the renderer's
    // SetInternalScale (its composite is rescaled into the new one by that frame, so a woven field survives), the
    // output images re-created at 704 n (the sync-mask growth path's Withdraw / wait / re-create), then the frame is
    // recorded and presented at n. A failed allocation keeps the previous n (logged). Without the renderer (software
    // renderer, copy-only output) it has no effect: those frames are native.
    void SetInternalScale(uint32_t n);
    // The internal resolution of the attached renderer (1 without one).
    uint32_t InternalScale() const;
    // The internal resolution of the last frame handed over (1 for the software renderer and the copy-only output).
    uint32_t PresentedScale() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace brimir::libretro
