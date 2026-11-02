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
// The image handed to set_image always has exactly the extent of the frame reported to video_cb (after crop, rotation
// and internal resolution, in both modes): a frontend samples the whole image as that frame. The output images are
// created for the current output size; a change (a resolution switch, a rotation or overscan change, an internal
// resolution change) creates images of the new size at that frame and retires the old ones (freed only after
// wait_sync_index for every sync index they were handed at, as for an internal resolution change below). Only images
// change with the size: the presenter's pipeline and per-slot buffers (gpu::PresenterCore) are created with the slots.
// Retired compute images whose wait completed are kept in a small cache (2 extents, at most 64 MiB, the current scale
// only), so a size change back (a resolution or interlace ping-pong) reuses them. When the new images cannot be created
// the frame is not handed over (the frontend repeats the image it holds, at its size; the identical request is retried
// after 1, 2, 4 ... 120 frames), or above 1x with a frame to record a lower internal resolution is used (logged). Below
// the requested internal resolution, the request is tried again (as a change) at the next native size change or 600,
// 1200, 2400 frames after the downgrade, at most 3 times until the option changes.
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
    // Returns false (and still detaches) when a wait failed, with error set. `discardState`: the emulation state is
    // being thrown away (content unloaded): VDP1 on the GPU is not read back first (VulkanVDPRenderer::DetachDevice).
    bool DetachRenderer(std::string &error, bool discardState = false);
    bool RendererAttached() const;

    // Renderer path: presents the renderer's frame of this retro_run (recorded into the slot's command buffer), or
    // re-presents the composite (no VDP2 frame ended in this run) as w x h with the output transforms, in one submit,
    // and hands the slot's image over. With VDP1 on the GPU the pending VDP1 ops are recorded into the same command
    // buffer (around the frame). Returns false when nothing was handed over; error is empty when there was just
    // nothing to show yet (a fresh composite), else says why (Mode() may have changed as with Present).
    bool PresentRenderer(uint32_t w, uint32_t h, const VulkanFrameSettings &settings, unsigned &outW, unsigned &outH,
                         std::string &error);
    // Renderer path with video disabled: records the renderer's frame (with the VDP1 ops around it) into the next
    // hidden context and submits it (no presenter, no set_image), at the renderer's current internal resolution.
    // Without a frame but with VDP1 ops pending (VDP1 on the GPU), it submits the ops alone: the ops-only submit, also
    // made for shown frames whose present did not take the ops (call it at the end of every retro_run). True when
    // there was nothing to record. The ops-only submit is never a second accepted submit of a retro_run: after an
    // accepted one the ops wait for the next retro_run (logged once); after a refused one it is made and logged
    // ("VDP1 ops-only submit after a refused frame submit").
    bool SubmitHidden(std::string &error);

    // The start of a retro_run (for the one-submit rule above), before the emulation: also takes the attached
    // renderer's completed VDP1 framebuffer readbacks (VulkanVDPRenderer::PollVdp1Readbacks).
    void BeginRun();
    // Whether GPU work outside the frame submits is allowed now: inside retro_run and the context callbacks only. The
    // renderer's VDP1 sync submits (VulkanVDPRenderer::SetSyncSubmit; a submit on a command buffer and fence of this
    // output, under the queue lock, then a wait) are refused otherwise. False until set.
    void SetGpuWorkAllowed(bool allowed);

    // Internal resolution n (1-4, brimir_internal_resolution) for the Vulkan VDP renderer. Kept across contexts: each
    // attach creates output images for it first (else for the highest scale below it that works) and attaches the
    // renderer at most at their scale (the renderer tries that, its previous effective scale, then 1; logged). A change applies at the next shown frame that has a VDP2 frame
    // to record (PresentRenderer), before it is recorded: wait for this output's slot and hidden-context fences (a
    // failed wait keeps the change pending), output images of n x the frame's size created, the renderer's
    // SetInternalScale (its composite is rescaled into the new one by that frame, so a woven field survives), then the
    // new images replace
    // the old ones, which are retired, not destroyed: each goes only after wait_sync_index (and this output's fence)
    // has returned for the sync index it was handed at, and never while it is the frontend's current image (its dupes
    // stay valid even if this frame's record fails). Either allocation failing keeps both at the previous n (logged);
    // the renderer's scale never exceeds the output images'. Without the renderer (software renderer, copy-only
    // output) it has no effect: those frames are native.
    void SetInternalScale(uint32_t n);
    // The internal resolution of the attached renderer (1 without one).
    uint32_t InternalScale() const;
    // The internal resolution of the last frame handed over (1 for the software renderer and the copy-only output).
    uint32_t PresentedScale() const;
    // VDP1 upscaling (brimir_vdp1_upscaling; VulkanVDPRenderer::SetVdp1Upscaling): kept across contexts and set on the
    // renderer at each attach. A change applies like an internal resolution change, at the next shown frame with a VDP2
    // frame to record, after the same waits (ApplyInternalScale): the output images keep their size and the composite
    // stays valid (the woven field too); the frame shows the sprites as drawn under the new setting.
    void SetVdp1Upscaling(bool enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace brimir::libretro
