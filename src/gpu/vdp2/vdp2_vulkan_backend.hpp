#pragma once

// Vulkan back half of the GPU VDP2 renderer: owns the VDP2 resources on the device (stage1b-port-inventory.md section
// 2.1) and replays a Vdp2FrameRecord from the front half (vdp2_front.hpp) into a command buffer with the three VDP2
// compute pipelines (section 2.4) and the NBG fetch pass. The frame lands in the persistent composite buffer (packed
// XBGR, pitch N * HRes at internal resolution N), which FramePresenter::PresentDevice reads.
//
// Internal resolution N (1-4, SetScale): the background layer image (NBG0-3, RBG0-1) and the composite are N x the
// native size and the bgs and compose passes run over N x N threads per native pixel; the sprite pass, the NBG fetch
// pass and every other image (sprite and mesh layers, sprite attributes, colour calculation window, RBG line colours)
// stay native. Only the resources of the current N are allocated. With VDP1 upscaling (SetSpriteSurfaceNx) an N x
// sprite pass also decodes the N x VDP1 surface into N x sprite layer and attribute images, which compose reads.
//
// Uploads go through frame contexts: each context owns a host-visible base upload block (4 MiB) plus overflow blocks
// chained on demand; the next time the context records, its overflow blocks are freed and its base block is reused, or
// reallocated to the total of a frame that overflowed (the high-water mark, so a steady frame size stops overflowing).
// The high-water mark is capped at kMaxUploadBlockSize and decays: after kHighWaterDecayRecords Records of the context
// in a row that fit a smaller block, it drops to the largest of them. A high-water block that cannot be allocated is
// logged once and the base block stays at the default size from then on (frames that need more chain overflow blocks).
// The caller guarantees that the GPU work of the context's previous Record has finished by then (its slot fence).
// Record never waits.

#include "gpu/vdp2/vdp2_front.hpp"
#include "gpu/vk_device.hpp"
#include "gpu/vk_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace brimir::gpu::vdp2 {

class Vdp2VulkanBackend {
public:
    static constexpr size_t kDefaultUploadBlockSize = 4 * 1024 * 1024;
    // The largest base upload block the high-water mark asks for (a bigger frame chains overflow blocks).
    static constexpr size_t kMaxUploadBlockSize = 32 * 1024 * 1024;
    // Records of a context in a row that fit a smaller base block before its high-water mark decays.
    static constexpr uint32_t kHighWaterDecayRecords = 120;

    // Creates every resource and pipeline, at internal resolution `scale` (1 to kMaxRenderScale): the resources that
    // scale are allocated for it directly; when they cannot be, the failure is logged (to `log`, stderr when empty)
    // and `fallbackScale` (when between 1 and scale), then 1 are tried, each allocated directly (Scale() says which
    // one it got). scaleAllocationLimitForTest: see SetScaleAllocationLimitForTest, applied from the start. Check Ok()
    // afterwards.
    Vdp2VulkanBackend(VulkanDevice &device, uint32_t frameContexts, uint32_t scale = 1,
                      VkDeviceSize scaleAllocationLimitForTest = 0,
                      std::function<void(const std::string &)> log = {}, uint32_t fallbackScale = 1);
    // Destroys everything immediately: the GPU must be done with all of it (e.g. after vkDeviceWaitIdle).
    ~Vdp2VulkanBackend();
    Vdp2VulkanBackend(const Vdp2VulkanBackend &) = delete;
    Vdp2VulkanBackend &operator=(const Vdp2VulkanBackend &) = delete;

    bool Ok() const { return m_result == VK_SUCCESS; }
    VkResult Result() const { return m_result; }
    uint32_t FrameContexts() const { return static_cast<uint32_t>(m_contexts.size()); }
    // Grows the number of frame contexts to at least `count` (their upload blocks are allocated at first use); every
    // other resource, the composite included, is kept. The caller re-maps context indices only after their previous
    // Records completed.
    void EnsureContexts(uint32_t count);

    // Copies the frame's snapshots into the context's upload blocks and records its uploads, dispatches and barriers
    // into cmd. Afterwards the composite buffer holds the frame (N*HRes x N*VRes, pitch N*HRes at internal resolution
    // N) and its writes are visible to compute shader and transfer reads recorded later. The context's previous Record
    // must have completed on the GPU.
    // Returns false and records nothing when the backend failed to initialize, context is out of range or an upload
    // block cannot be allocated (the caller then re-uploads everything with the next frame; the frame's composite clear
    // stays pending for the next Record).
    bool Record(const Vdp2FrameRecord &frame, uint32_t context, VkCommandBuffer cmd);

    // The persistent composite: uint[704N * 512N] of packed XBGR (0xAABBGGRR) at internal resolution N, pitch N * HRes,
    // STORAGE | TRANSFER_SRC | TRANSFER_DST.
    VkBuffer CompositeBuffer() const { return m_composite.buffer; }

    // The sprite framebuffer (kSpriteFBBufferSize bytes in Ymir's slice layout; STORAGE | TRANSFER_SRC |
    // TRANSFER_DST), which the sprite pass reads at the display framebuffer. In GPU VDP1 mode it is the GPU FBRAM:
    // the VDP1 back half writes it (Vdp1VulkanBackend) and the frames carry no sprite slices.
    VkBuffer SpriteFBBuffer() const { return m_spriteFB.buffer; }
    // External: someone else (the VDP1 back half) initializes and writes the sprite framebuffer, so this back half
    // never zeroes it (call before the first Record; a pending zero fill is dropped).
    void SetSpriteFBExternal(bool external) {
        m_spriteFBExternal = external;
        if (external) {
            m_pending.zeroSpriteFB = false;
        }
    }

    // Internal resolution n (1 to kMaxRenderScale) from the next Record on. The resources that scale (the background
    // layer image and the composite) are reallocated here for n only, the new ones before the old ones go. The caller
    // guarantees that no work uses the old ones any more: every submitted command buffer that uses them has completed,
    // and no command buffer recorded by Record is still waiting to be submitted (its descriptor sets are rewritten
    // here; a recorded command buffer that will not be submitted must be reported with FrameDropped first).
    // The old composite is kept until the next Record, which first rescales it into the new one (nearest: the sample
    // points (n x, n y) keep the old (m x, m y)), so a woven field survives the change; that rescale replaces the
    // clear a new composite would get, and the frame's own clears come after it. Without anything recorded yet, or
    // with a clear already pending, there is nothing to keep and the new composite is cleared (to black, or the
    // pending colour) instead. A second SetScale before that Record keeps rescaling from the composite of the last
    // Record.
    // When the new resources cannot be allocated, the previous resources and scale are kept, the failure is logged and
    // false is returned. Also false (nothing changes) for an n out of range or a backend that failed to initialize.
    // N x sprite layers, if any (SetSpriteSurfaceNx), are freed first (the caller sets them up again for n).
    bool SetScale(uint32_t n);
    uint32_t Scale() const { return m_scale; }
    // Device memory the resources that scale take at internal resolution n.
    static VkDeviceSize ScaledResourceBytes(uint32_t n);

    // VDP1 upscaling (GPU stage 3; stage3-research.md section 3.2): `surface` is the N x VDP1 surface
    // (Vdp1VulkanBackend::NxSurfaceBuffer at N = Scale() > 1). From the next Record on, every batch with layer lines also
    // runs the N x sprite pass, which decodes it into N x sprite layer and attribute images, and compose reads those at
    // the N x pixel (CommonRenderParams::spriteScale); the native sprite pass still runs and feeds the sprite window of
    // bgs and the fetch pass. VK_NULL_HANDLE frees the N x sprite images: compose reads the native sprite layers again.
    // The images are allocated here (free first: old ones go before new ones are created); on failure nothing N x is
    // left, the failure is logged and false returned. Also false at Scale() 1 or for a backend that failed to
    // initialize. The caller's guarantee is SetScale's (descriptor sets are rewritten).
    bool SetSpriteSurfaceNx(VkBuffer surface);
    // The N x VDP1 surface the N x sprite pass decodes (VK_NULL_HANDLE: none, native sprite layers).
    VkBuffer SpriteSurfaceNx() const { return m_spriteSurfaceNx; }
    // Device memory of the N x sprite layer and attribute images at internal resolution n.
    static VkDeviceSize SpriteNxImageBytes(uint32_t n);
    // Test hook: SetSpriteSurfaceNx's allocations fail.
    void FailSpriteNxAllocationForTest(bool fail) { m_failSpriteNx = fail; }

    // Where failures (scale and large upload block allocations) are logged; stderr when unset.
    void SetLog(std::function<void(const std::string &)> log) { m_log = std::move(log); }

    // The command buffer of the last successful Record will not be submitted (or was discarded): the one-time work it
    // carried (a composite clear or rescale, zeroing the sprite framebuffer) is pending again for the next Record (the
    // rescale's previous composite is kept for it). Its uploads are
    // lost too; the caller makes the front half re-upload everything (Vdp2FrontEnd::MarkAllDirty).
    void FrameDropped();

    // Size of the base upload block of every context below its high-water mark (reallocated at each context's next
    // Record).
    void ForceUploadBlockSizeForTest(size_t bytes);
    // SetScale fails (as an allocation failure) when the resources of the new scale take more than `bytes` (0: no
    // limit).
    void SetScaleAllocationLimitForTest(VkDeviceSize bytes) { m_scaleAllocationLimit = bytes; }
    // Base upload blocks above the default size (high-water blocks) fail to allocate.
    void FailHighWaterBlocksForTest(bool fail) { m_failHighWaterBlocks = fail; }
    // kHighWaterDecayRecords for this backend.
    void SetHighWaterDecayRecordsForTest(uint32_t records) { m_highWaterDecayRecords = std::max<uint32_t>(records, 1); }
    // Fill the background and sprite layer, sprite attribute, RBG line colour and colour calculation window images and
    // the NBG character table with 0xA5 bytes at the start of every frame, so reads of texels or entries the frame did
    // not write show up as differences.
    void SetPoisonImagesForTest(bool enable) { m_poisonImages = enable; }
    // A composite clear will be recorded by the next Record.
    bool CompositeClearPendingForTest() const { return m_pending.clearComposite; }
    // A rescale of the previous composite will be recorded by the next Record.
    bool RescalePendingForTest() const { return m_pending.rescale; }
    // The native frame size the composite holds (the last batch of the last successful Record; 0 before any).
    uint32_t CompositeWidthForTest() const { return m_compositeWidth; }
    uint32_t CompositeHeightForTest() const { return m_compositeHeight; }
    // Upload blocks the context used for its last Record (1 = no overflow).
    uint32_t UploadBlocksForTest(uint32_t context) const;
    // What the last successful Record uploaded.
    struct RecordStats {
        VkDeviceSize uploadBytes = 0;    // bytes written into upload blocks (per-line arrays and the arena)
        VkDeviceSize vramCopyBytes = 0;  // bytes copied into VRAM
        VkDeviceSize cramCopyBytes = 0;  // bytes copied into the CRAM colour and coefficient buffers
        uint32_t overflowBlocks = 0;     // overflow blocks created (chained behind the context's base block)
        VkDeviceSize baseBlockSize = 0;  // size of the context's base block
        VkDeviceSize highWater = 0;      // the context's high-water mark after the Record (0: none)
        uint32_t scale = 0;              // internal resolution of the Record
        uint32_t fetchPasses = 0;        // NBG fetch dispatches
        uint32_t fetchOutputSize = 0;    // outputSize the last NBG fetch dispatch pushed (always native)
        uint32_t composeOutputSize = 0;  // outputSize the last compose dispatch pushed (always native)
        uint32_t nxSpritePasses = 0;     // N x sprite pass dispatches (VDP1 upscaling)
        uint32_t nxComposes = 0;         // compose dispatches that read the N x sprite layers
    };
    const RecordStats &LastRecordStatsForTest() const { return m_lastStats; }

private:
    // The rescale pipeline (kRescale) has its own descriptor set (m_rescaleSet), not one per upload block.
    enum Pipeline : uint32_t { kSprite, kBgs, kCompose, kNBGFetch, kPipelineCount, kRescale = kPipelineCount };

    struct UploadBlock {
        vk_util::Buffer buffer; // host-visible, coherent, persistently mapped
        VkDeviceSize used = 0;
        VkDescriptorPool pool = VK_NULL_HANDLE;              // created with the sets, on the first arena placement
        std::array<VkDescriptorSet, kPipelineCount> sets{}; // parameter bindings point into this block
        // VDP1 upscaling: the N x sprite pass's and compose's sets on the N x sprite images (the native images while
        // there are none)
        VkDescriptorSet nxSpriteSet = VK_NULL_HANDLE;
        VkDescriptorSet nxComposeSet = VK_NULL_HANDLE;
    };
    struct FrameContext {
        std::vector<UploadBlock> blocks;
        // Bytes the context's last Record needed in one block, kept when it overflowed: its next base block is at
        // least this big, so a steady frame size stops creating overflow blocks.
        VkDeviceSize highWater = 0;
        // Records in a row whose block size (BlockSizeFor) was below the high-water block, and the largest of them.
        uint32_t smallRecords = 0;
        VkDeviceSize smallPeak = 0;
        // A previous composite the context's last Record rescaled from: freed when the context records again (its
        // work has completed then).
        vk_util::Buffer retiredComposite;
    };
    struct Allocation {
        size_t block = 0; // index into FrameContext::blocks
        VkDeviceSize offset = 0;
    };

    VkResult Init(uint32_t frameContexts, uint32_t scale, uint32_t fallbackScale);
    void Destroy();
    void DestroyBlock(UploadBlock &block);
    void Log(const std::string &text) const;
    // The background layer image and the composite for internal resolution n.
    VkResult CreateScaledResources(uint32_t n, vk_util::Image &bgLayers, vk_util::Buffer &composite);
    // A base block that holds `bytes` in one piece (the high-water rounding).
    VkDeviceSize BlockSizeFor(VkDeviceSize bytes) const;
    VkDeviceSize BaseBlockSize(const FrameContext &context) const;
    void ResetContext(FrameContext &context);
    // After a Record that needed `bytes` in one block: grows (it overflowed), keeps or decays the high-water mark.
    void UpdateHighWater(FrameContext &context, VkDeviceSize bytes, bool overflowed);
    bool AddBlock(FrameContext &context, VkDeviceSize size);
    bool Allocate(FrameContext &context, VkDeviceSize size, VkDeviceSize alignment, Allocation &out);
    VkResult CreateBlockSets(UploadBlock &block);
    // Points the block's descriptor sets at the current resources.
    void WriteBlockSets(UploadBlock &block);
    void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                 VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) const;
    // `scale` is the dispatch's renderScale (1 for the native sprite pass), `spriteScale` its spriteScale (N in the
    // N x sprite pass and the compose dispatches reading the N x sprite layers, with `set` their N x set; 0 otherwise,
    // with the block's set of the pipeline).
    void Dispatch(VkCommandBuffer cmd, Pipeline pipeline, const UploadBlock &block, uint32_t paramsOffset,
                  const VDP2CommonRenderParams &common, uint32_t startY, uint32_t lines, uint32_t groupsX,
                  uint32_t groupsZ, uint32_t outputSize, uint32_t scale, uint32_t spriteScale = 0,
                  VkDescriptorSet set = VK_NULL_HANDLE) const;
    void DestroySpriteNx();
    void WriteAllBlockSets();
    // The NBG fetch pass over `lines` dispatch rows from startY (one workgroup per row and NBG), native at every
    // internal resolution: outputSize is the native size.
    void DispatchNBGFetch(VkCommandBuffer cmd, const UploadBlock &block, uint32_t paramsOffset,
                          const VDP2CommonRenderParams &common, uint32_t startY, uint32_t lines,
                          uint32_t outputSize) const;

    VulkanDevice &m_device;
    VkResult m_result = VK_ERROR_INITIALIZATION_FAILED;
    VkDeviceSize m_paramAlignment = 256;
    size_t m_blockSize = kDefaultUploadBlockSize;
    uint32_t m_scale = 1;
    std::function<void(const std::string &)> m_log;
    // A high-water block failed to allocate (logged): base blocks stay at m_blockSize from then on.
    bool m_highWaterFailed = false;
    uint32_t m_highWaterDecayRecords = kHighWaterDecayRecords;
    VkDeviceSize m_scaleAllocationLimit = 0;
    bool m_failHighWaterBlocks = false;

    // Device-local resources (bindings in vdp2_bindings.hpp)
    vk_util::Buffer m_vram;
    vk_util::Buffer m_cramColors;
    vk_util::Buffer m_cramCoeffs;
    vk_util::Buffer m_rotBases;
    vk_util::Buffer m_lnclBack;
    vk_util::Buffer m_nbgLines;
    vk_util::Buffer m_spriteFB;
    vk_util::Buffer m_composite;    // uint[704N * 512N]
    vk_util::Buffer m_nbgChars;     // uint[4 * 512 * 704], the NBG character table
    vk_util::Image m_bgLayers;      // R8G8B8A8_UINT 704Nx512N, 6 layers (NBG0-3, RBG0-1)
    vk_util::Image m_spriteLayers;  // R8G8B8A8_UINT 704x512, 2 layers (sprite, mesh)
    vk_util::Image m_spriteAttrs;   // R32_UINT 704x512, 2 layers
    vk_util::Image m_rbgLineColor;  // R8G8B8A8_UINT 352x512, 2 layers
    vk_util::Image m_colorCalcWindow; // R32_UINT 704x512
    // VDP1 upscaling (SetSpriteSurfaceNx): the N x VDP1 surface (not owned) and the N x sprite images
    VkBuffer m_spriteSurfaceNx = VK_NULL_HANDLE;
    vk_util::Image m_spriteLayersNx; // R8G8B8A8_UINT 704Nx512N, 2 layers (sprite, mesh)
    vk_util::Image m_spriteAttrsNx;  // R32_UINT 704Nx512N, 2 layers
    bool m_failSpriteNx = false;

    // One-time work for the next Record (a new back half clears its composite to black and zeroes the sprite
    // framebuffer; after a scale change the previous composite is rescaled into the new one, before any clear), and
    // what the last successful Record recorded (re-armed by FrameDropped).
    struct PendingWork {
        bool clearComposite = false;
        uint32_t clearColor = 0xFF000000u;
        bool zeroSpriteFB = false;
        bool rescale = false;
    };
    PendingWork m_pending{.clearComposite = true, .zeroSpriteFB = true};
    PendingWork m_recorded{};
    uint32_t m_lastContext = 0; // the context of the last successful Record

    // The previous composite while a rescale is pending: its scale and the native frame size it holds.
    struct RescaleSource {
        vk_util::Buffer composite;
        uint32_t scale = 1;
        uint32_t width = 0, height = 0;
    };
    RescaleSource m_rescaleSource;
    // The native frame size the composite holds.
    uint32_t m_compositeWidth = 0, m_compositeHeight = 0;
    VkDescriptorSetLayout m_rescaleSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_rescalePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_rescalePipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_rescalePool = VK_NULL_HANDLE;
    VkDescriptorSet m_rescaleSet = VK_NULL_HANDLE;
    bool m_poisonImages = false;
    bool m_spriteFBExternal = false;
    RecordStats m_lastStats{};
    uint32_t m_overflowBlocks = 0; // created by the current Record

    std::array<VkDescriptorSetLayout, kPipelineCount> m_setLayouts{};
    std::array<VkPipelineLayout, kPipelineCount> m_pipelineLayouts{};
    std::array<VkPipeline, kPipelineCount> m_pipelines{};

    std::vector<FrameContext> m_contexts;
};

} // namespace brimir::gpu::vdp2
