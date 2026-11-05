#include "vulkan_vdp_renderer.hpp"

#include "gpu/vdp2/vdp2_bindings.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>

namespace brimir::gpu {

using namespace ymir;
using namespace ymir::vdp;

namespace {

// Adds the scope's duration to `seconds` when `on` (FrameProbe).
class ProbeTimer {
public:
    ProbeTimer(bool on, double &seconds) : m_seconds(on ? &seconds : nullptr) {
        if (m_seconds != nullptr) {
            m_start = std::chrono::steady_clock::now();
        }
    }
    ~ProbeTimer() {
        if (m_seconds != nullptr) {
            *m_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
        }
    }
    ProbeTimer(const ProbeTimer &) = delete;
    ProbeTimer &operator=(const ProbeTimer &) = delete;

private:
    double *m_seconds;
    std::chrono::steady_clock::time_point m_start{};
};

constexpr uint32_t kSliceSize = vdp1::kFbramSliceSize;
static_assert(sizeof(SpriteFB) == kSliceSize);
// Slice masks (vdp1_hw_params.hpp: main 0-1, alt 2-3, mesh[alt][fb] 4 + 2 * alt + fb)
constexpr uint32_t kMainSlices = 0x03;
constexpr uint32_t kAltSlices = 0x0C;
constexpr uint32_t kMeshSlices = 0xF0;

} // namespace

VulkanVDPRenderer::VulkanVDPRenderer(VDPState &state, config::VDP2DebugRender &vdp2DebugRenderOptions,
                                     const config::VDP2AccessPatternsConfig &vdp2AccessPatternsConfig)
    : HardwareVDPRendererBase(VDPRendererType::External)
    , m_state(state)
    , m_vdp1(std::make_unique<SoftwareVDPRenderer>(state, vdp2DebugRenderOptions, vdp2AccessPatternsConfig))
    , m_front(state, vdp2AccessPatternsConfig)
    , m_vdp1Front(state)
    , m_meshShadow(std::make_unique<MeshFBRAM>())
    , m_altShadow(std::make_unique<std::array<SpriteFB, 2>>()) {
    for (auto &alt : *m_meshShadow) {
        for (auto &fb : alt) {
            fb.fill(0);
        }
    }
}

VulkanVDPRenderer::~VulkanVDPRenderer() {
    // The owner waited for the device (or abandoned it) before destroying the renderer.
    DestroyGpuVdp1();
}

// -----------------------------------------------------------------------------
// Device

void VulkanVDPRenderer::AttachDevice(VulkanDevice &device, uint32_t frameContexts, uint32_t maxScale) {
    if (m_backend) {
        LeaveGpuVdp1(true);
    }
    m_backend.reset();
    // The requested scale (capped), else the previous effective one, else 1: the back half tries them in that order,
    // each allocated directly (failures logged by it).
    const uint32_t cap = std::clamp<uint32_t>(maxScale, 1, vdp2::kMaxRenderScale);
    uint32_t wanted = std::min(m_requestedScale, cap);
    const uint32_t previous = std::min(m_internalScale, cap);
    const bool gpuVdp1 = m_vdp1Mode == Vdp1Mode::Gpu;
    // Gpu VDP1 mode: one more upload context, the sync submits'
    const uint32_t contexts = frameContexts + (gpuVdp1 ? 1u : 0u);
    for (;;) {
        auto backend = std::make_unique<vdp2::Vdp2VulkanBackend>(device, contexts, wanted, m_scaleAllocationLimit,
                                                                 m_log, previous < wanted ? previous : 1);
        if (!backend->Ok()) {
            return;
        }
        m_internalScale = backend->Scale();
        m_backend = std::move(backend);
        m_frameContexts = frameContexts;
        // A new back half has none of the state: everything is uploaded with the next frame, and its composite starts
        // cleared (Vdp2VulkanBackend clears it on first use).
        m_front.ConsumeFrame();
        m_front.MarkAllDirty();
        VkResult failure = VK_SUCCESS;
        if (!gpuVdp1 || EnterGpuVdp1(device, contexts, failure)) {
            break;
        }
        // Only a GPU short of memory tries a lower internal resolution; any other failure keeps this one with VDP1 in
        // software
        const bool outOfMemory = failure == VK_ERROR_OUT_OF_DEVICE_MEMORY || failure == VK_ERROR_OUT_OF_HOST_MEMORY;
        if (m_internalScale <= 1 || !outOfMemory) {
            Log("VDP1 on the GPU unavailable; VDP1 stays in software");
            break;
        }
        // The VDP1 back half did not fit next to this internal resolution's resources (a GPU short of memory): a lower
        // internal resolution, so VDP1 stays on the GPU (nothing was recorded with this back half yet)
        Log("VDP1 on the GPU unavailable at " + std::to_string(m_internalScale) + "x; trying " +
            std::to_string(m_internalScale - 1) + "x");
        wanted = m_internalScale - 1;
        m_backend.reset();
    }
    if (m_internalScale != m_requestedScale) {
        Log("VDP2 internal resolution: requested " + std::to_string(m_requestedScale) + "x, using " +
            std::to_string(m_internalScale) + "x");
    }
}

void VulkanVDPRenderer::DetachDevice(bool discardState) {
    LeaveGpuVdp1(!discardState);
    m_front.ConsumeFrame();
    m_front.MarkAllDirty();
    m_backend.reset();
}

bool VulkanVDPRenderer::EnsureFrameContexts(uint32_t frameContexts) {
    if (!m_backend) {
        return false;
    }
    // The sync context moves up with the caller's count (its work has always completed and been merged: every sync
    // waits).
    const uint32_t extra = m_gpuVdp1 ? 1u : 0u;
    m_backend->EnsureContexts(frameContexts + extra);
    if (m_vdp1Backend) {
        m_vdp1Backend->EnsureContexts(frameContexts + extra);
    }
    m_frameContexts = std::max(m_frameContexts, frameContexts);
    if (m_contextFences.size() < m_frameContexts + extra) {
        m_contextFences.resize(m_frameContexts + extra, VK_NULL_HANDLE);
    }
    return true;
}

void VulkanVDPRenderer::AbandonDevice() {
    m_front.ConsumeFrame();
    m_front.MarkAllDirty();
    if (m_gpuVdp1) {
        // No Vulkan call and no read of the readback buffers (their memory may be gone with the device): software
        // continues from the shadow as of the last merge (rule 8).
        HandToSoftware();
    }
    (void)m_vdp1Backend.release();
    (void)m_passProfile.release();
    if (m_backend) {
        m_backend->SetPassProfile(nullptr);
    }
    NotifyVdp1Rescale(0);
    m_device = nullptr;
    m_gpuVdp1 = false;
    m_vdp1Queue.clear();
    m_vdp1Recorded.clear();
    m_vdp2FrameRecorded = false;
    m_vdp1BeforeFrame = 0;
    m_proposed = {};
    m_contextFences.clear();
    (void)m_vdp1Front.TakeOps();
    (void)m_backend.release();
}

void VulkanVDPRenderer::EnableThreadedVDP1(bool enable) {
    m_vdp1->EnableThreadedVDP1(enable);
    m_threadedVDP1 = enable;
}

bool VulkanVDPRenderer::HasPendingFrame() const {
    return m_backend != nullptr && m_front.HasFrame();
}

bool VulkanVDPRenderer::RecordPendingFrame(uint32_t context, VkCommandBuffer cmd) {
    if (m_gpuVdp1) {
        bool frameRecorded = false;
        RecordVdp1Work(context, cmd, FramePolicy::Record, frameRecorded);
        return frameRecorded;
    }
    m_vdp1Recorded.clear();
    m_vdp2FrameRecorded = false;
    m_lastNxStats = {};
    if (!HasPendingFrame()) {
        return false;
    }
    const bool recorded = m_backend->Record(m_front.Frame(), context, cmd);
    TakeNxRecordStats(recorded);
    m_vdp2FrameRecorded = recorded;
    m_front.ConsumeFrame();
    if (!recorded) {
        // The frame's snapshots never reached the GPU.
        m_front.MarkAllDirty();
    }
    return recorded;
}

bool VulkanVDPRenderer::HasPendingVdp1Work() const {
    if (!m_gpuVdp1) {
        return false;
    }
    if (!m_vdp1Queue.empty() || m_vdp1Front.HasPendingOps()) {
        return true;
    }
    // A new N x surface waiting for its rebuild
    if (m_vdp1Backend->NxScale() != 0 && m_vdp1Backend->HasPendingWork()) {
        return true;
    }
    // A GpuNewer slice whose readback no command buffer took yet (rule 2: none may outlive the retro_run)
    for (uint32_t slice = 0; slice < 8; ++slice) {
        if (((ShadowedSlices() >> slice) & 1u) && NeedsReadback(slice, m_vdp1Seq)) {
            return true;
        }
    }
    return false;
}

bool VulkanVDPRenderer::RecordPendingVdp1Work(uint32_t context, VkCommandBuffer cmd) {
    if (!HasPendingVdp1Work()) {
        m_vdp1Recorded.clear();
        m_vdp1OneTimeRecorded = false;
        m_vdp2FrameRecorded = false;
        m_lastNxStats = {};
        return false;
    }
    bool frameRecorded = false;
    return RecordVdp1Work(context, cmd, FramePolicy::Keep, frameRecorded) && RecordedVdp1Work();
}

bool VulkanVDPRenderer::TakeFrameRecordedBySync() {
    const bool recorded = m_frameRecordedBySync;
    m_frameRecordedBySync = false;
    return recorded;
}

void VulkanVDPRenderer::RecordedFrameDropped() {
    // Only a dropped command buffer that holds a VDP2 frame re-arms its one-time work: after a frame a sync submit
    // recorded (and submitted), the backend's last Record is not the dropped one.
    if (m_backend && m_vdp2FrameRecorded) {
        m_backend->FrameDropped();
    }
    m_vdp2FrameRecorded = false;
    m_front.MarkAllDirty();
    if (m_vdp1Backend) {
        m_vdp1Backend->FrameDropped();
    }
    if (m_passProfile) {
        m_passProfile->Dropped();
    }
    m_proposed = {}; // its readbacks never happen
    m_vdp1OneTimeRecorded = false; // re-armed by the back half's FrameDropped
    if (!m_vdp1Recorded.empty()) {
        // The dropped ops come before everything still pending (they were recorded first).
        const size_t count = m_vdp1Recorded.size();
        m_vdp1Queue.insert(m_vdp1Queue.begin(), std::make_move_iterator(m_vdp1Recorded.begin()),
                           std::make_move_iterator(m_vdp1Recorded.end()));
        m_vdp1Recorded.clear();
        m_vdp1BeforeFrame += count;
    }
}

VkBuffer VulkanVDPRenderer::CompositeBuffer() const {
    return m_backend ? m_backend->CompositeBuffer() : VK_NULL_HANDLE;
}

bool VulkanVDPRenderer::SetInternalScale(uint32_t n) {
    if (n < 1 || n > vdp2::kMaxRenderScale) {
        return false;
    }
    m_requestedScale = n;
    m_vdp1NxSuspendedAt = 0; // a new request: VDP1 upscaling is tried again
    return FallBackInternalScale(n);
}

bool VulkanVDPRenderer::FallBackInternalScale(uint32_t n) {
    if (n < 1 || n > vdp2::kMaxRenderScale) {
        return false;
    }
    if (BackHalfLost()) {
        return false; // logged when it was lost
    }
    if (m_backend) {
        // VDP1 upscaling: the N x sprite surface of the old scale is freed before the VDP2 resources of the new one are
        // allocated, and the new one allocated after them (its contents are rebuilt from the GPU FBRAM).
        const bool nxChanges = m_vdp1Backend && m_vdp1Backend->NxScale() != Vdp1NxTarget(n);
        if (nxChanges) {
            ApplyVdp1NxScale(0);
        }
        if (!m_backend->SetScale(n)) {
            if (m_backend->Ok()) {
                if (nxChanges) {
                    ApplyVdp1NxScale(Vdp1NxTarget(m_internalScale));
                }
            } else {
                // Not even the previous scale's layers again: nothing can be recorded with this back half any more
                // (the N x sprite surface stays freed). The owner detaches the renderer (BackHalfLost).
                Log("VDP2 back half lost at the internal resolution change; the renderer has to be attached again");
            }
            return false; // logged by the back half, which keeps its scale
        }
        m_internalScale = n;
        if (nxChanges) {
            ApplyVdp1NxScale(Vdp1NxTarget(n));
        }
        return true;
    }
    m_internalScale = n;
    return true;
}

uint32_t VulkanVDPRenderer::Vdp1NxTarget(uint32_t n) const {
    return m_vdp1Upscaling && n > 1 && n != m_vdp1NxSuspendedAt ? n : 0;
}

bool VulkanVDPRenderer::SuspendVdp1Upscaling() {
    if (Vdp1UpscaleScale() == 0) {
        return false;
    }
    m_vdp1NxSuspendedAt = m_internalScale;
    ApplyVdp1NxScale(0);
    Log("VDP1 upscaling off at " + std::to_string(m_internalScale) + "x: the GPU is short of memory");
    return true;
}

uint32_t VulkanVDPRenderer::Vdp1UpscaleScale() const {
    return m_vdp1Backend ? m_vdp1Backend->NxScale() : 0;
}

void VulkanVDPRenderer::ApplyVdp1NxScale(uint32_t scale) {
    if (!m_vdp1Backend) {
        return;
    }
    if (m_vdp1Backend->NxScale() != scale) {
        // Free first: VDP2's N x sprite images, then the old surface (SetNxScale)
        if (m_backend) {
            m_backend->SetSpriteSurfaceNx(VK_NULL_HANDLE);
        }
        if (!m_vdp1Backend->SetNxScale(scale)) {
            Log("VDP1 upscaling: the " + std::to_string(scale) + "x sprite surface could not be allocated (" +
                vk_util::ResultName(m_vdp1Backend->NxFailure()) + "); VDP1 upscaling off at this internal resolution");
        } else if (scale != 0) {
            Log("VDP1 upscaling: sprite surface at " + std::to_string(scale) + "x (" +
                std::to_string(m_vdp1Backend->NxMemoryBytes() / (1024 * 1024)) + " MiB)");
        }
        NotifyVdp1Rescale(m_vdp1Backend->NxScale());
    }
    SyncVdp2NxSprites();
}

void VulkanVDPRenderer::SyncVdp2NxSprites() {
    if (!m_backend) {
        return;
    }
    const uint32_t nx = m_vdp1Backend ? m_vdp1Backend->NxScale() : 0;
    const VkBuffer surface =
        m_vdp2NxSprites && nx > 1 && nx == m_backend->Scale() ? m_vdp1Backend->NxSurfaceBuffer() : VK_NULL_HANDLE;
    if (m_backend->SpriteSurfaceNx() == surface) {
        return;
    }
    if (!m_backend->SetSpriteSurfaceNx(surface)) {
        // Logged by the back half: no N x sprites at this scale, and no surface nobody shows
        Log("VDP1 upscaling off at this internal resolution");
        m_vdp1Backend->SetNxScale(0);
        NotifyVdp1Rescale(0);
    }
}

bool VulkanVDPRenderer::Vdp2NxSprites() const {
    return m_backend && m_backend->SpriteSurfaceNx() != VK_NULL_HANDLE;
}

void VulkanVDPRenderer::SetVdp2NxSpritesForTest(bool enabled) {
    m_vdp2NxSprites = enabled;
    SyncVdp2NxSprites();
}

void VulkanVDPRenderer::TakeNxRecordStats(bool frameRecorded) {
    m_lastNxStats = {};
    if (frameRecorded && m_backend) {
        m_lastNxStats.spritePasses = m_backend->LastRecordStatsForTest().nxSpritePasses;
    }
    if (m_vdp1Backend && m_vdp1Backend->NxScale() != 0) {
        const auto &s = m_vdp1Backend->StatsForTest();
        m_lastNxStats.vdp1Passes = s.nxSubBatches + s.nxErases + s.nxFbWriteRuns + s.nxRebuilds;
    }
}

void VulkanVDPRenderer::NotifyVdp1Rescale(uint32_t scale) {
    if (m_vdp1WorkObserver && (scale != 0 || m_vdp1NxNotified != 0)) {
        Vdp1WorkEvent event;
        event.kind = Vdp1WorkEvent::Kind::Rescale;
        event.scale = scale;
        m_vdp1WorkObserver(event);
    }
    m_vdp1NxNotified = scale;
}

void VulkanVDPRenderer::SetVdp1Upscaling(bool enabled) {
    m_vdp1Upscaling = enabled;
    m_vdp1NxSuspendedAt = 0;
    if (m_backend) {
        ApplyVdp1NxScale(Vdp1NxTarget(m_internalScale));
    }
}

void VulkanVDPRenderer::SetVdp1NxAllocationLimitForTest(VkDeviceSize bytes) {
    m_vdp1NxAllocationLimit = bytes;
    if (m_vdp1Backend) {
        m_vdp1Backend->SetNxAllocationLimitForTest(bytes);
    }
}

void VulkanVDPRenderer::SetVdp1OverwritePath(bool enabled) {
    m_vdp1OverwritePath = enabled;
    if (m_vdp1Backend) {
        m_vdp1Backend->SetNxOverwritePath(enabled);
    }
}

void VulkanVDPRenderer::SetLog(std::function<void(const std::string &)> log) {
    m_log = std::move(log);
    if (m_backend) {
        m_backend->SetLog(m_log);
    }
}

void VulkanVDPRenderer::SetScaleAllocationLimitForTest(VkDeviceSize bytes) {
    m_scaleAllocationLimit = bytes;
    if (m_backend) {
        m_backend->SetScaleAllocationLimitForTest(bytes);
    }
}

void VulkanVDPRenderer::Log(const std::string &text) const {
    if (m_log) {
        m_log(text);
    } else {
        std::fprintf(stderr, "[vulkan] %s\n", text.c_str());
    }
}

// -----------------------------------------------------------------------------
// Gpu VDP1 mode

bool VulkanVDPRenderer::EnterGpuVdp1(VulkanDevice &device, uint32_t contexts, VkResult &failure) {
    if (m_vdp1FailureForTest != VK_SUCCESS) {
        failure = m_vdp1FailureForTest;
        return false;
    }
    auto backend = std::make_unique<vdp1::Vdp1VulkanBackend>(device, contexts, m_backend->SpriteFBBuffer());
    if (!backend->Ok()) {
        failure = backend->Result();
        return false;
    }
    backend->SetLog([this](const std::string &text) {
        if (!m_readbackMemoryLogged) { // once per renderer, not per back half
            m_readbackMemoryLogged = true;
            Log(text);
        }
    });
    m_device = &device;
    m_vdp1Backend = std::move(backend);
    m_backend->SetSpriteFBExternal(true);
    if (PassProfileProbe) {
        auto profile = std::make_unique<GpuPassProfile>(device);
        if (profile->Ok()) {
            m_passProfile = std::move(profile);
            m_vdp1Backend->SetPassProfile(m_passProfile.get());
            m_backend->SetPassProfile(m_passProfile.get());
        } else {
            Log("GPU pass profile: no timestamps on this queue");
        }
    }

    // The software VDP1 state becomes the GPU's: FBRAM (VDPState main, the inner renderer's alt and mesh), doubleV,
    // and the front half's VRAM copy (uploaded whole with the first batch).
    m_vdp1->PreSaveStateSync();
    auto saved = std::make_unique<savestate::VDPSaveState::VDPRendererSaveState>();
    m_vdp1->SaveState(*saved);
    *m_meshShadow = saved->vdp1State.meshFBRAM;
    m_vdp1Front.SetEnhancements(m_enhancements);
    m_vdp1Front.SetDoubleV(saved->vdp1State.doubleV != 0);
    (void)m_vdp1Front.TakeOps(); // the front half was not fed in Software mode
    m_vdp1Front.PostLoadStateSync();
    m_vdp1Queue.clear();
    m_vdp1Recorded.clear();
    m_vdp1BeforeFrame = 0;
    m_contextFences.assign(contexts, VK_NULL_HANDLE);
    ResetShadow();
    (*m_altShadow)[0] = m_vdp1->GetAltFBRAM(0);
    (*m_altShadow)[1] = m_vdp1->GetAltFBRAM(1);
    QueueFbramUpload(0xFF, &(*m_altShadow)[0], &(*m_altShadow)[1]);
    m_gpuVdp1 = true;
    m_frameRecordedBySync = false;
    // VDP1 upscaling: the N x surface, rebuilt from the uploaded FBRAM
    m_vdp1Backend->SetNxAllocationLimitForTest(m_vdp1NxAllocationLimit);
    m_vdp1Backend->SetNxOverwritePath(m_vdp1OverwritePath);
    ApplyVdp1NxScale(Vdp1NxTarget(m_internalScale));
    return true;
}

void VulkanVDPRenderer::LeaveGpuVdp1(bool sync) {
    if (m_gpuVdp1) {
        // The caller waited for the device: every committed readback is complete. Then one sync submit for what the
        // GPU holds beyond them (alt included, shadowed or not), so software continues from the GPU's framebuffers.
        OwnWorkCompleted();
        if (sync && !SyncShadow(kMainSlices | kMeshSlices | kAltSlices, Vdp1SyncKind::Detach, FramePolicy::Ignore)) {
            Log("VDP1: reading the GPU framebuffers back failed; VDP1 continues in software from older ones");
        }
        HandToSoftware();
    }
    DestroyGpuVdp1();
}

void VulkanVDPRenderer::HandToSoftware() {
    // VDPState holds main (the shadow); the inner renderer gets doubleV (its own section loaded back with it), mesh
    // and alt, then copies VDPState into a threaded VDP1's own state (PostLoadStateSync), as after a state load.
    m_vdp1->PreSaveStateSync();
    auto saved = std::make_unique<savestate::VDPSaveState::VDPRendererSaveState>();
    m_vdp1->SaveState(*saved);
    saved->vdp1State.doubleV = m_vdp1Front.DoubleV() ? 1 : 0;
    m_vdp1->LoadState(*saved);
    for (uint8_t fb = 0; fb < 2; ++fb) {
        m_vdp1->SetMeshFBRAM(false, fb, (*m_meshShadow)[0][fb]);
        m_vdp1->SetMeshFBRAM(true, fb, (*m_meshShadow)[1][fb]);
        m_vdp1->SetAltFBRAM(fb, (*m_altShadow)[fb]);
    }
    m_vdp1->PostLoadStateSync();
}

void VulkanVDPRenderer::DestroyGpuVdp1() {
    if (m_backend) {
        m_backend->SetSpriteSurfaceNx(VK_NULL_HANDLE); // the N x surface dies with the VDP1 back half
        m_backend->SetPassProfile(nullptr);
    }
    m_vdp1Backend.reset();
    m_passProfile.reset();
    NotifyVdp1Rescale(0); // the N x surface dies with the back half
    m_device = nullptr;
    m_gpuVdp1 = false;
    m_vdp1Queue.clear();
    m_vdp1Recorded.clear();
    m_vdp1BeforeFrame = 0;
    m_proposed = {};
    m_contextFences.clear();
    (void)m_vdp1Front.TakeOps();
    if (m_backend) {
        m_backend->SetSpriteFBExternal(false);
    }
}

void VulkanVDPRenderer::QueueVdp1Ops() {
    if (m_vdp1Front.HasPendingOps()) {
        m_vdp1Queue.push_back({m_vdp1Front.TakeOps(), {}, 0, m_vdp1Seq});
        if (m_opDumpCollecting) {
            m_opDump->lists.push_back(m_vdp1Queue.back().ops);
        }
    } else {
        (void)m_vdp1Front.TakeOps(); // op-less frame boundaries
    }
}

void VulkanVDPRenderer::QueueFbramUpload(uint32_t sliceMask, const SpriteFB *alt0, const SpriteFB *alt1) {
    QueueVdp1Ops(); // the ops so far come first
    if (m_opDumpCollecting) {
        Log("VDP1 op dump: abandoned (an FBRAM upload during the captured frame)");
        m_opDump.reset();
        m_opDumpCollecting = false;
    }
    Vdp1Work work;
    work.sliceMask = sliceMask;
    work.seq = m_vdp1Seq;
    work.fbram.assign(vdp1::kFbramBufferSize, 0);
    const auto put = [&](uint32_t slice, const SpriteFB *fb) {
        if (fb != nullptr && ((sliceMask >> slice) & 1u)) {
            std::memcpy(work.fbram.data() + size_t{slice} * kSliceSize, fb->data(), kSliceSize);
        }
    };
    put(vdp1::Vdp1MainSlice(0), &m_state.mem1.FBRAM[0]);
    put(vdp1::Vdp1MainSlice(1), &m_state.mem1.FBRAM[1]);
    put(vdp1::Vdp1AltSlice(0), alt0);
    put(vdp1::Vdp1AltSlice(1), alt1);
    for (uint32_t alt = 0; alt < 2; ++alt) {
        for (uint32_t fb = 0; fb < 2; ++fb) {
            put(vdp1::Vdp1MeshSlice(alt, fb), &(*m_meshShadow)[alt][fb]);
        }
    }
    m_vdp1Queue.push_back(std::move(work));
}

bool VulkanVDPRenderer::ReplayVdp1Work(const Vdp1Work &work, uint32_t context, VkCommandBuffer cmd) {
    ProbeTimer timer(FrameProbe != nullptr, m_vdp1Times.recordSeconds);
    if (work.sliceMask != 0) {
        return m_vdp1Backend->RecordFbramUpload(work.fbram, work.sliceMask, context, cmd);
    }
    const vdp1::Vdp1Segment all{0, static_cast<uint32_t>(work.ops.ops.size())};
    return m_vdp1Backend->Record(work.ops, all, context, cmd);
}

bool VulkanVDPRenderer::RecordVdp1Work(uint32_t context, VkCommandBuffer cmd, FramePolicy policy,
                                       bool &frameRecorded, uint32_t extra) {
    frameRecorded = false;
    m_vdp1OneTimeRecorded = false;
    m_vdp1Recorded.clear(); // the previous command buffer was submitted (or reported dropped)
    m_vdp2FrameRecorded = false;
    m_proposed = {};
    m_lastNxStats = {};
    if (FrameProbe && !m_vdp1Backend->EnableTimestamps() && !m_timestampsLogged) {
        // Diagnostics: vdp1_gpu_us stays 0
        Log("VDP1 probe: no GPU timestamps on this queue; the VDP1 GPU time is not measured");
        m_timestampsLogged = true;
    }
    // This context's previous command buffer has completed: its readbacks can be merged before the buffer is reused.
    MergeContext(context);
    if (!m_vdp1Backend->BeginContext(context)) {
        return false;
    }
    if (m_passProfile) {
        m_passProfile->Begin(context);
    }
    // A new N x surface's rebuild, even without ops (the surface is current at the end of every command buffer)
    m_vdp1OneTimeRecorded = m_vdp1Backend->NxScale() != 0 && m_vdp1Backend->RecordPendingWork(context, cmd);
    const bool framePending = m_front.HasFrame();
    const size_t before = framePending && policy != FramePolicy::Ignore
                              ? std::min(m_vdp1BeforeFrame, m_vdp1Queue.size())
                              : m_vdp1Queue.size();
    const bool after = m_vdp1Queue.size() > before || m_vdp1Front.HasPendingOps();
    const bool recordFrame =
        framePending && (policy == FramePolicy::Record || (policy == FramePolicy::Sync && after));
    // The ops after the frame's end (all of them without a frame) go in this command buffer too.
    const bool replayAfter = recordFrame || !framePending || policy == FramePolicy::Ignore;

    size_t done = 0;
    bool ok = true;
    const auto replay = [&](size_t end) {
        for (; ok && done < end; ++done) {
            ok = ReplayVdp1Work(m_vdp1Queue[done], context, cmd);
            if (!ok) {
                break;
            }
        }
    };
    replay(before);
    if (ok && recordFrame) {
        frameRecorded = m_backend->Record(m_front.Frame(), context, cmd);
        m_vdp2FrameRecorded = frameRecorded;
        m_front.ConsumeFrame();
        if (!frameRecorded) {
            m_front.MarkAllDirty(); // the frame's snapshots never reached the GPU
        }
    }
    if (ok && replayAfter) {
        QueueVdp1Ops();
        replay(m_vdp1Queue.size());
    }
    // The emissions this command buffer covers, with the earlier committed ones (one queue: they run first).
    uint64_t covered = m_submittedSeq;
    if (done > 0) {
        covered = std::max(covered, m_vdp1Queue[done - 1].seq);
    }
    if (done == m_vdp1Queue.size() && !m_vdp1Front.HasPendingOps()) {
        covered = m_vdp1Seq;
    }
    // What was recorded leaves the queue (RecordedFrameDropped puts it back); what failed stays, in order.
    m_vdp1Recorded.assign(std::make_move_iterator(m_vdp1Queue.begin()),
                          std::make_move_iterator(m_vdp1Queue.begin() + static_cast<std::ptrdiff_t>(done)));
    m_vdp1Queue.erase(m_vdp1Queue.begin(), m_vdp1Queue.begin() + static_cast<std::ptrdiff_t>(done));
    m_vdp1BeforeFrame = m_vdp1BeforeFrame > done ? m_vdp1BeforeFrame - done : 0;
    if (!ok) {
        Log("VDP1: recording the VDP1 ops failed (upload space); they stay pending");
        m_vdp1RecordFailed = true;
    }

    // Rule 2: the readbacks of the GpuNewer shadowed slices this command buffer covers, plus `extra` (whatever they
    // hold, when covered).
    const uint32_t shadowedSlices = ShadowedSlices();
    uint32_t mask = 0;
    for (uint32_t slice = 0; slice < 8; ++slice) {
        const bool shadowed = (shadowedSlices >> slice) & 1u;
        const bool wanted = (extra >> slice) & 1u;
        const SliceSync &s = m_slices[slice];
        if ((shadowed && NeedsReadback(slice, covered)) ||
            (wanted && s.lastTouch <= covered && s.lastWrite <= covered)) {
            mask |= 1u << slice;
        }
    }
    m_proposed.context = static_cast<int32_t>(context);
    m_proposed.covered = covered;
    if (mask != 0) {
        ProbeTimer timer(FrameProbe != nullptr, m_vdp1Times.recordSeconds);
        if (m_vdp1Backend->RecordReadback(context, mask, cmd)) {
            m_proposed.mask = mask;
            for (uint32_t slice = 0; slice < 8; ++slice) {
                m_proposed.seq[slice] = m_slices[slice].lastTouch;
            }
        } else {
            Log("VDP1: the framebuffer readback buffer cannot be allocated; the CPU copy falls behind");
        }
    }
    TakeNxRecordStats(frameRecorded);
    return ok;
}

// -----------------------------------------------------------------------------
// CPU shadow

uint32_t VulkanVDPRenderer::ShadowedSlices() const {
    return kMainSlices | kMeshSlices | (m_enhancements.deinterlace ? kAltSlices : 0u);
}

void VulkanVDPRenderer::ClearPending(SliceSync &s) {
    for (PendingReadback &p : s.pending) {
        if (p.mask) {
            if (p.mask->any) {
                p.mask->bits.fill(0);
                p.mask->any = false;
            }
            m_maskPool.push_back(std::move(p.mask));
        }
    }
    s.pending.clear();
}

void VulkanVDPRenderer::ResetShadow() {
    for (SliceSync &s : m_slices) {
        ClearPending(s);
        s.lastTouch = s.lastWrite = s.valid = m_vdp1Seq;
    }
    m_submittedSeq = m_vdp1Seq;
    m_proposed = {};
}

void VulkanVDPRenderer::MarkShadowValid(uint32_t mask) {
    for (uint32_t slice = 0; slice < 8; ++slice) {
        if ((mask >> slice) & 1u) {
            SliceSync &s = m_slices[slice];
            s.valid = m_vdp1Seq;
            ClearPending(s);
        }
    }
}

void VulkanVDPRenderer::Touch(uint32_t slice) {
    m_slices[slice].lastTouch = m_vdp1Seq;
}

void VulkanVDPRenderer::CpuWrite(uint32_t address, uint32_t bytes) {
    ++m_vdp1Seq;
    const uint32_t fb = m_state.fbIndex.draw & 1u;
    SliceSync &s = m_slices[vdp1::Vdp1MainSlice(fb)];
    s.lastWrite = m_vdp1Seq;
    // Every committed readback that is not merged yet lacks this write: its merge keeps the CPU's bytes.
    for (PendingReadback &p : s.pending) {
        for (uint32_t i = 0; i < bytes; ++i) {
            const uint32_t byte = (address + i) & (kSliceSize - 1);
            p.mask->bits[byte >> 6] |= uint64_t{1} << (byte & 63);
        }
        p.mask->any = true;
    }
    if (m_enhancements.deinterlace && m_state.regs2.TVMD.IsInterlaced()) {
        Touch(vdp1::Vdp1AltSlice(fb)); // the mirror (software's VDP1WriteFB), an op on the GPU
    }
}

bool VulkanVDPRenderer::NeedsReadback(uint32_t slice, uint64_t covered) const {
    const SliceSync &s = m_slices[slice];
    const uint64_t known = std::max(s.valid, s.pending.empty() ? 0 : s.pending.back().seq);
    // Every op and CPU write into the slice must be in the command buffer (the readback holds the writes so far).
    return s.lastTouch > known && s.lastTouch <= covered && s.lastWrite <= covered;
}

void VulkanVDPRenderer::CommitReadback(uint32_t context) {
    if (m_proposed.context != static_cast<int32_t>(context)) {
        return;
    }
    m_submittedSeq = std::max(m_submittedSeq, m_proposed.covered);
    for (uint32_t slice = 0; slice < 8; ++slice) {
        if (((m_proposed.mask >> slice) & 1u) == 0) {
            continue;
        }
        ++m_syncStats.readbackSlices;
        SliceSync &s = m_slices[slice];
        // A context holds one readback: an older entry of it was merged when the context began this command buffer.
        PendingReadback entry;
        entry.seq = m_proposed.seq[slice];
        entry.context = context;
        if (slice < 2) {
            if (m_maskPool.empty()) {
                entry.mask = std::make_unique<WriteMask>();
            } else {
                entry.mask = std::move(m_maskPool.back());
                m_maskPool.pop_back();
            }
        }
        s.pending.push_back(std::move(entry));
    }
    m_proposed = {};
}

void VulkanVDPRenderer::MergeEntry(uint32_t slice, size_t index) {
    SliceSync &s = m_slices[slice];
    const PendingReadback &p = s.pending[index];
    const uint8_t *src = m_vdp1Backend && p.seq > s.valid ? m_vdp1Backend->ReadbackData(p.context, slice) : nullptr;
    if (src != nullptr) {
        const auto start = std::chrono::steady_clock::now();
        if (slice < 2) {
            // Main: bytes the CPU wrote since the readback was committed keep the CPU's value.
            uint8_t *dst = m_state.mem1.FBRAM[slice].data();
            const WriteMask &mask = *p.mask;
            if (!mask.any) {
                std::memcpy(dst, src, kSliceSize);
            } else {
                for (uint32_t block = 0; block < kSliceSize / 64; ++block) {
                    const uint64_t bits = mask.bits[block];
                    const uint32_t base = block * 64;
                    if (bits == 0) {
                        std::memcpy(dst + base, src + base, 64);
                        continue;
                    }
                    for (uint32_t i = 0; i < 64; ++i) {
                        if (((bits >> i) & 1u) == 0) {
                            dst[base + i] = src[base + i];
                        }
                    }
                }
            }
        } else if (slice < 4) {
            std::memcpy((*m_altShadow)[slice - vdp1::Vdp1AltSlice(0)].data(), src, kSliceSize);
        } else {
            const uint32_t mesh = slice - vdp1::Vdp1MeshSlice(0, 0);
            std::memcpy((*m_meshShadow)[mesh >> 1][mesh & 1].data(), src, kSliceSize);
        }
        s.valid = p.seq;
        ++m_syncStats.mergedSlices;
        m_syncStats.mergeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    // It and every older readback are done with (the older ones are behind it).
    for (size_t i = 0; i <= index; ++i) {
        PendingReadback &old = s.pending[i];
        if (old.mask) {
            if (old.mask->any) {
                old.mask->bits.fill(0);
                old.mask->any = false;
            }
            m_maskPool.push_back(std::move(old.mask));
        }
    }
    s.pending.erase(s.pending.begin(), s.pending.begin() + static_cast<std::ptrdiff_t>(index + 1));
}

void VulkanVDPRenderer::MergeContext(uint32_t context) {
    if (!m_gpuVdp1 || m_vdp1Backend == nullptr) {
        return;
    }
    for (uint32_t slice = 0; slice < 8; ++slice) {
        SliceSync &s = m_slices[slice];
        for (size_t i = s.pending.size(); i-- > 0;) {
            if (s.pending[i].context == context) {
                MergeEntry(slice, i);
                break;
            }
        }
    }
}

void VulkanVDPRenderer::WaitContext(uint32_t context, Vdp1SyncKind kind) {
    const auto start = std::chrono::steady_clock::now();
    const VkFence fence = context < m_contextFences.size() ? m_contextFences[context] : VK_NULL_HANDLE;
    if (fence != VK_NULL_HANDLE && m_device != nullptr) {
        const VkResult result = vk_util::WaitForFence(*m_device, fence); // polled, then blocking
        ++m_syncStats.fenceWaits[static_cast<uint32_t>(kind)];
        if (result != VK_SUCCESS) {
            Log("VDP1: waiting for a framebuffer readback failed; the CPU copy stays behind");
            return;
        }
        m_contextFences[context] = VK_NULL_HANDLE;
    }
    MergeContext(context);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    m_syncStats.waitSeconds += seconds;
    m_syncStats.maxWaitSeconds = std::max(m_syncStats.maxWaitSeconds, seconds);
}

void VulkanVDPRenderer::FrameSubmitted(uint32_t context, VkFence fence) {
    m_vdp2FrameRecorded = false;
    if (!m_gpuVdp1) {
        return;
    }
    if (context >= m_contextFences.size()) {
        m_contextFences.resize(context + 1, VK_NULL_HANDLE);
    }
    m_contextFences[context] = fence;
    if (m_vdp1WorkObserver) {
        for (const Vdp1Work &work : m_vdp1Recorded) {
            Vdp1WorkEvent event;
            event.kind = work.sliceMask != 0 ? Vdp1WorkEvent::Kind::Upload : Vdp1WorkEvent::Kind::Ops;
            event.ops = &work.ops;
            event.fbram = work.fbram;
            event.sliceMask = work.sliceMask;
            m_vdp1WorkObserver(event);
        }
    }
    m_vdp1Recorded.clear(); // submitted: nothing to put back
    m_vdp1OneTimeRecorded = false;
    CommitReadback(context);
    if (fence == VK_NULL_HANDLE) {
        MergeContext(context);
    }
}

void VulkanVDPRenderer::OwnWorkCompleted() {
    if (!m_gpuVdp1) {
        return;
    }
    for (uint32_t context = 0; context < m_contextFences.size(); ++context) {
        m_contextFences[context] = VK_NULL_HANDLE;
    }
    // Every readback is complete: the newest of each slice is merged (the others are behind it).
    for (uint32_t slice = 0; slice < 8; ++slice) {
        if (!m_slices[slice].pending.empty()) {
            MergeEntry(slice, m_slices[slice].pending.size() - 1);
        }
    }
}

void VulkanVDPRenderer::PollVdp1Readbacks() {
    if (!m_gpuVdp1 || m_device == nullptr) {
        return;
    }
    // Contexts whose last command buffer has completed (bit i), each looked at once
    uint64_t looked = 0, done = 0;
    const auto completed = [&](uint32_t context) {
        if (context >= 64) {
            return false;
        }
        const uint64_t bit = uint64_t{1} << context;
        if ((looked & bit) == 0) {
            looked |= bit;
            const VkFence fence = context < m_contextFences.size() ? m_contextFences[context] : VK_NULL_HANDLE;
            if (fence == VK_NULL_HANDLE ||
                m_device->Table().vkGetFenceStatus(m_device->Device(), fence) == VK_SUCCESS) {
                done |= bit;
            }
        }
        return (done & bit) != 0;
    };
    for (uint32_t slice = 0; slice < 8; ++slice) {
        SliceSync &s = m_slices[slice];
        for (size_t i = s.pending.size(); i-- > 0;) {
            if (completed(s.pending[i].context)) {
                MergeEntry(slice, i); // the newest completed one
                break;
            }
        }
    }
}

bool VulkanVDPRenderer::SyncSubmit(Vdp1SyncKind kind, FramePolicy policy, uint32_t extra) {
    if (!m_syncSubmit || m_vdp1Backend == nullptr) {
        ++m_syncStats.refusedSyncs;
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const uint32_t context = m_frameContexts; // the sync context: its previous command buffer has completed
    bool frameRecorded = false;
    bool called = false;
    bool recorded = false;
    const bool submitted = m_syncSubmit([&](VkCommandBuffer cmd) {
        called = true;
        (void)RecordVdp1Work(context, cmd, policy, frameRecorded, extra);
        // A partial failure still submits what was recorded (in order; the rest stays queued).
        recorded = frameRecorded || RecordedVdp1Work();
        return recorded;
    });
    if (!submitted) {
        if (recorded) {
            // Not submitted (refused, or a failed submit): the ops are queued again. A submit whose wait then failed
            // also lands here and would replay its ops twice; that only happens with a lost device, whose work is
            // gone anyway.
            RecordedFrameDropped();
        }
        if (!called || recorded) {
            ++m_syncStats.refusedSyncs; // nothing recorded (a recording failure, logged) is not a refusal
        }
        return false;
    }
    m_frameRecordedBySync |= frameRecorded;
    FrameSubmitted(context, VK_NULL_HANDLE); // complete: commits and merges its readbacks
    ++m_syncStats.syncSubmits[static_cast<uint32_t>(kind)];
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    m_syncStats.syncSeconds += seconds;
    m_syncStats.maxSyncSeconds = std::max(m_syncStats.maxSyncSeconds, seconds);
    return true;
}

bool VulkanVDPRenderer::SyncShadow(uint32_t mask, Vdp1SyncKind kind, FramePolicy policy) {
    // GpuNewer slices need the sync submit (it also reads back every other covered GpuNewer slice); Pending ones a
    // wait for their fence.
    uint32_t behind = 0;
    bool needSubmit = false;
    for (uint32_t slice = 0; slice < 8; ++slice) {
        const SliceSync &s = m_slices[slice];
        if (((mask >> slice) & 1u) == 0 || s.valid >= s.lastTouch) {
            continue;
        }
        behind |= 1u << slice;
        needSubmit = needSubmit || s.pending.empty() || s.pending.back().seq < s.lastTouch;
    }
    if (behind == 0) {
        return true;
    }
    if (needSubmit) {
        (void)SyncSubmit(kind, policy, behind);
    }
    bool current = true;
    for (uint32_t slice = 0; slice < 8; ++slice) {
        const SliceSync &s = m_slices[slice];
        if (((behind >> slice) & 1u) == 0 || s.valid >= s.lastTouch) {
            continue;
        }
        if (!s.pending.empty() && s.pending.back().seq >= s.lastTouch) {
            WaitContext(s.pending.back().context, kind);
        }
        current = current && s.valid >= s.lastTouch;
    }
    if (!current && kind != Vdp1SyncKind::Save) { // PreSaveStateSync logs its own (once)
        Log("VDP1: the GPU framebuffers could not be read back here; the CPU copy is older");
    }
    return current;
}

uint64_t VulkanVDPRenderer::Vdp1SyncSubmits() const {
    uint64_t total = 0;
    for (const uint64_t n : m_syncStats.syncSubmits) {
        total += n;
    }
    return total;
}

// -----------------------------------------------------------------------------
// Basics and configuration

void VulkanVDPRenderer::Reset(bool hard) {
    m_vdp1->Reset(hard);
    m_front.Reset(hard);
    m_HRes = kDefaultResH;
    m_VRes = kDefaultResV;
    m_exclusiveMonitor = false;
    m_displayEraseSinceCapture = false;
    if (m_gpuVdp1) {
        // VDPState was reset first (VDP::Reset): a hard reset cleared main FBRAM (a soft one keeps it: the GPU's stays
        // authoritative). Software clears the alt and mesh framebuffers at every reset (sw:94-136). Rule 7: the ops so
        // far are replayed first, then the uploads of zeros (and main from VDPState); the erases after this use the
        // reset m_VRes above, as software's.
        m_vdp1Front.Reset(hard);
        for (auto &alt : *m_meshShadow) {
            for (auto &fb : alt) {
                fb.fill(0);
            }
        }
        for (auto &fb : *m_altShadow) {
            fb.fill(0);
        }
        const uint32_t slices = kAltSlices | kMeshSlices | (hard ? kMainSlices : 0u);
        QueueFbramUpload(slices, nullptr, nullptr);
        MarkShadowValid(slices);
    }
}

void VulkanVDPRenderer::UpdateEnhancements() {
    m_vdp1->ConfigureEnhancements(m_enhancements);
    m_front.SetEnhancements(m_enhancements);
    m_vdp1Front.SetEnhancements(m_enhancements);
}

// -----------------------------------------------------------------------------
// Save states (the renderer section is the software renderer's, hybrid section 4; in Gpu VDP1 mode doubleV and the
// mesh framebuffers are the front half's and the shadow's)

void VulkanVDPRenderer::PreSaveStateSync() {
    m_vdp1->PreSaveStateSync();
    // Rule 5: fence waits in retro_serialize; a sync submit only for a save inside retro_run with newer GPU data.
    // The alt slices too while they are shadowed: the save state extension holds them (SaveBrimirState).
    const bool current = !m_gpuVdp1 || SyncShadow(kMainSlices | kMeshSlices | (ShadowedSlices() & kAltSlices),
                                                  Vdp1SyncKind::Save, FramePolicy::Sync);
    m_saveSyncFailed = !current || m_failSaveSync;
    if (m_saveSyncFailed && !m_saveSyncFailLogged) {
        Log("VDP1: the GPU framebuffers could not be read back before a save; a save state fails rather than hold "
            "older framebuffers");
        m_saveSyncFailLogged = true;
    }
}

void VulkanVDPRenderer::PostLoadStateSync() {
    m_vdp1->PostLoadStateSync();
    m_front.PostLoadStateSync();
    if (m_gpuVdp1) {
        // Rule 6: the ops before the load happened (their alt writes stay, as software's), then the loaded
        // framebuffers.
        QueueVdp1Ops();
        m_vdp1Front.PostLoadStateSync();
        if (m_stateLoaded) {
            // Rule 6: a slice whose loaded bytes equal what the GPU holds keeps its N x detail (no upload, no rebuild)
            uint32_t keep = m_preLoadValid;
            for (uint32_t fb = 0; fb < 2; ++fb) {
                const uint32_t slice = vdp1::Vdp1MainSlice(fb);
                if (((keep >> slice) & 1u) && m_state.mem1.FBRAM[fb] != (*m_preLoadMain)[fb]) {
                    keep &= ~(1u << slice);
                }
            }
            const uint32_t upload = (kMainSlices | kMeshSlices) & ~keep;
            // The state's alt framebuffers (LoadBrimirState), unless the GPU holds the same bytes
            const uint32_t altUpload = m_altLoaded ? kAltSlices & ~keep : 0u;
            if ((upload | altUpload) != 0) {
                QueueFbramUpload(upload | altUpload, &(*m_altShadow)[0], &(*m_altShadow)[1]);
            }
            MarkShadowValid(kMainSlices | kMeshSlices | (m_altLoaded ? ShadowedSlices() & kAltSlices : 0u));
            ++m_syncStats.stateLoads;
            m_syncStats.loadUploadedSlices += static_cast<uint32_t>(std::popcount(upload));
            m_syncStats.loadKeptSlices += static_cast<uint32_t>(std::popcount((kMainSlices | kMeshSlices) & keep));
        }
    }
    m_stateLoaded = false;
    m_altLoaded = false;
    m_preLoadValid = 0;
}

void VulkanVDPRenderer::PreLoadState() {
    m_preLoadValid = 0;
    if (!m_gpuVdp1 || m_stateLoaded) {
        return; // (a LoadState without its PostLoadStateSync changed the mesh shadow)
    }
    // Valid: the shadow holds every op emitted so far and every CPU write, which is what the GPU holds once the
    // queued and submitted work has run (no wait here: GpuNewer and Pending slices are uploaded as before).
    const uint32_t candidates = kMainSlices | kMeshSlices | (ShadowedSlices() & kAltSlices);
    for (uint32_t slice = 0; slice < 8; ++slice) {
        const SliceSync &s = m_slices[slice];
        if (((candidates >> slice) & 1u) && s.valid >= s.lastTouch) {
            m_preLoadValid |= 1u << slice;
        }
    }
    for (uint32_t fb = 0; fb < 2; ++fb) {
        if ((m_preLoadValid >> vdp1::Vdp1MainSlice(fb)) & 1u) {
            if (!m_preLoadMain) {
                m_preLoadMain = std::make_unique<std::array<SpriteFB, 2>>();
            }
            (*m_preLoadMain)[fb] = m_state.mem1.FBRAM[fb];
        }
    }
}

void VulkanVDPRenderer::SaveState(savestate::VDPSaveState::VDPRendererSaveState &state) {
    m_vdp1->SaveState(state);
    if (m_gpuVdp1) {
        state.vdp1State.doubleV = m_vdp1Front.DoubleV() ? 1 : 0;
        state.vdp1State.meshFBRAM = *m_meshShadow;
    }
    // The NBG fetchers are the front half's (the inner renderer draws no VDP2 lines).
    m_front.SaveFetchers(state);
}

bool VulkanVDPRenderer::ValidateState(const savestate::VDPSaveState::VDPRendererSaveState &state) const {
    return m_vdp1->ValidateState(state);
}

void VulkanVDPRenderer::LoadState(const savestate::VDPSaveState::VDPRendererSaveState &state) {
    m_vdp1->LoadState(state);
    m_front.LoadFetchers(state);
    if (m_gpuVdp1) {
        m_vdp1Front.SetDoubleV(state.vdp1State.doubleV != 0);
        if (m_stateLoaded) {
            m_preLoadValid = 0; // a second LoadState: the mesh shadow is the first one's
        }
        for (uint32_t alt = 0; alt < 2; ++alt) {
            for (uint32_t fb = 0; fb < 2; ++fb) {
                const uint32_t bit = 1u << vdp1::Vdp1MeshSlice(alt, fb);
                if ((m_preLoadValid & bit) && (*m_meshShadow)[alt][fb] != state.vdp1State.meshFBRAM[alt][fb]) {
                    m_preLoadValid &= ~bit;
                }
            }
        }
        *m_meshShadow = state.vdp1State.meshFBRAM;
        m_stateLoaded = true;
    }
}

void VulkanVDPRenderer::SaveBrimirState(savestate::BrimirSaveState::VDPRenderer &state) {
    if (m_gpuVdp1) {
        // PreSaveStateSync read the shadowed alt slices back; unshadowed (deinterlacing off) they are not drawn.
        for (size_t fb = 0; fb < 2; ++fb) {
            std::memcpy(state.altFBRAM[fb].data(), (*m_altShadow)[fb].data(), kSliceSize);
        }
        // The main shadow holds every op so far (PreSaveStateSync): the draw framebuffer as drawn so far.
        std::memcpy(state.vdp1DrawFB.data(), m_state.mem1.FBRAM[m_state.fbIndex.draw].data(), kSliceSize);
    } else {
        m_vdp1->SaveBrimirState(state);
    }
}

void VulkanVDPRenderer::LoadBrimirState(const savestate::BrimirSaveState::VDPRenderer &state) {
    m_vdp1->LoadBrimirState(state);
    if (m_gpuVdp1) {
        // Rule 6, as for main and mesh: an alt slice whose loaded bytes equal what the GPU holds keeps its N x detail.
        for (size_t fb = 0; fb < 2; ++fb) {
            const uint32_t bit = 1u << vdp1::Vdp1AltSlice(static_cast<uint32_t>(fb));
            if (std::memcmp((*m_altShadow)[fb].data(), state.altFBRAM[fb].data(), kSliceSize) != 0) {
                m_preLoadValid &= ~bit;
                std::memcpy((*m_altShadow)[fb].data(), state.altFBRAM[fb].data(), kSliceSize);
            }
        }
        m_altLoaded = true; // uploaded by PostLoadStateSync unless kept
        // The GPU continues from the draw framebuffer as drawn so far (a state saved by a threaded software VDP1 in the
        // middle of a draw holds an older one in FBRAM); PostLoadStateSync uploads it with the main slices.
        std::memcpy(m_state.mem1.FBRAM[m_state.fbIndex.draw].data(), state.vdp1DrawFB.data(), kSliceSize);
    }
}

// -----------------------------------------------------------------------------
// VDP1 (software, or the front half in Gpu mode)

void VulkanVDPRenderer::VDP1WriteVRAM(uint32_t address, uint8_t value) {
    if (m_gpuVdp1) {
        m_vdp1Front.WriteVRAM(address);
        return;
    }
    m_vdp1->VDP1WriteVRAM(address, value);
}

void VulkanVDPRenderer::VDP1WriteVRAM(uint32_t address, uint16_t value) {
    if (m_gpuVdp1) {
        m_vdp1Front.WriteVRAM(address);
        return;
    }
    m_vdp1->VDP1WriteVRAM(address, value);
}

void VulkanVDPRenderer::VDP1SyncFB() {
    if (m_gpuVdp1) {
        // Rule 4: the CPU reads the draw framebuffer's main slice.
        (void)SyncShadow(1u << vdp1::Vdp1MainSlice(m_state.fbIndex.draw & 1u), Vdp1SyncKind::Read, FramePolicy::Sync);
        return;
    }
    m_vdp1->VDP1SyncFB();
}

void VulkanVDPRenderer::VDP1DebugSyncFB() {
    if (m_gpuVdp1) {
        return; // a no-op, as software's
    }
    m_vdp1->VDP1DebugSyncFB();
}

void VulkanVDPRenderer::VDP1WriteFB(uint32_t address, uint8_t value) {
    if (m_gpuVdp1) {
        ProbeTimer timer(FrameProbe != nullptr, m_vdp1Times.frontSeconds);
        m_vdp1Front.WriteFB8(address, value);
        CpuWrite(address, 1);
        return;
    }
    m_vdp1->VDP1WriteFB(address, value);
}

void VulkanVDPRenderer::VDP1WriteFB(uint32_t address, uint16_t value) {
    if (m_gpuVdp1) {
        ProbeTimer timer(FrameProbe != nullptr, m_vdp1Times.frontSeconds);
        m_vdp1Front.WriteFB16(address, value);
        CpuWrite(address & ~1u, 2);
        return;
    }
    m_vdp1->VDP1WriteFB(address, value);
}

void VulkanVDPRenderer::VDP1WriteReg(uint32_t address, uint16_t value) {
    if (m_gpuVdp1) {
        return; // the front half reads the registers live at command time
    }
    m_vdp1->VDP1WriteReg(address, value);
}

void VulkanVDPRenderer::VDP1EraseFramebuffer(uint64_t cycles) {
    if (m_gpuVdp1) {
        ProbeTimer timer(FrameProbe != nullptr, m_vdp1Times.frontSeconds);
        // Rule 1: the display framebuffer's main, its alt mirror and its meshes, as VDP1DoEraseFramebuffer writes them
        ++m_vdp1Seq;
        const uint32_t fb = m_state.fbIndex.display & 1u;
        const bool mirror = m_enhancements.deinterlace && m_state.regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity;
        Touch(vdp1::Vdp1MainSlice(fb));
        if (mirror) {
            Touch(vdp1::Vdp1AltSlice(fb));
        }
        if (m_enhancements.transparentMeshes) {
            Touch(vdp1::Vdp1MeshSlice(0, fb));
            if (mirror) {
                Touch(vdp1::Vdp1MeshSlice(1, fb));
            }
        }
        m_vdp1Front.Erase(cycles, m_VRes);
        return;
    }
    m_vdp1->VDP1EraseFramebuffer(cycles);
    // A threaded erase is queued, not waited for; the capture syncs unless a swap (which waits) comes first.
    m_displayEraseSinceCapture = true;
}

void VulkanVDPRenderer::VDP1SwapFramebuffer() {
    if (!m_gpuVdp1) {
        m_vdp1->VDP1SwapFramebuffer();
        m_displayEraseSinceCapture = false;
    }
    // Gpu mode: a swap is only fbIndex (the front half reads it live; batches close at a draw framebuffer change).
    Callbacks.VDP1FramebufferSwap();
}

void VulkanVDPRenderer::VDP1BeginFrame() {
    if (m_gpuVdp1) {
        ProbeTimer timer(FrameProbe != nullptr, m_vdp1Times.frontSeconds);
        m_vdp1Front.BeginFrame();
        return;
    }
    m_vdp1->VDP1BeginFrame();
}

void VulkanVDPRenderer::VDP1ExecuteCommand(uint32_t cmdAddress, VDP1Command::Control control) {
    if (m_gpuVdp1) {
        ProbeTimer timer(FrameProbe != nullptr, m_vdp1Times.frontSeconds);
        if (static_cast<uint16_t>(control.command) <= 0x7) {
            // Rule 1: a draw command plots into the draw framebuffer's main (alt with deinterlacing, VDP1PlotPixel's
            // altFB) and, with transparent meshes, its meshes
            ++m_vdp1Seq;
            const uint32_t fb = m_state.fbIndex.draw & 1u;
            Touch(vdp1::Vdp1MainSlice(fb));
            if (m_enhancements.deinterlace) {
                Touch(vdp1::Vdp1AltSlice(fb));
            }
            if (m_enhancements.transparentMeshes) {
                Touch(vdp1::Vdp1MeshSlice(0, fb));
                if (m_enhancements.deinterlace) {
                    Touch(vdp1::Vdp1MeshSlice(1, fb));
                }
            }
        }
        m_vdp1Front.ExecuteCommand(cmdAddress, control);
        return;
    }
    m_vdp1->VDP1ExecuteCommand(cmdAddress, control);
}

void VulkanVDPRenderer::VDP1EndFrame() {
    if (!m_gpuVdp1) {
        m_vdp1->VDP1EndFrame();
    }
    Callbacks.VDP1DrawFinished();
}

void VulkanVDPRenderer::DumpExtraVDP1Framebuffers(std::ostream &out) const {
    if (!m_gpuVdp1) {
        m_vdp1->DumpExtraVDP1Framebuffers(out);
        return;
    }
    // Gpu mode (debug only): one sync of main, mesh and alt (as a CPU read; alt into its shadow), then software's
    // layout (sw:610-623)
    auto &self = const_cast<VulkanVDPRenderer &>(*this);
    (void)self.SyncShadow(kMainSlices | kMeshSlices | kAltSlices, Vdp1SyncKind::Dump, FramePolicy::Sync);
    const uint8_t drawFB = m_state.fbIndex.draw & 1u;
    const uint8_t dispFB = m_state.fbIndex.display & 1u;
    if (m_enhancements.deinterlace) {
        out.write(reinterpret_cast<const char *>((*m_altShadow)[drawFB].data()), kSliceSize);
        out.write(reinterpret_cast<const char *>((*m_altShadow)[dispFB].data()), kSliceSize);
    }
    if (m_enhancements.transparentMeshes) {
        for (const uint32_t alt : {0u, 1u}) {
            out.write(reinterpret_cast<const char *>((*m_meshShadow)[alt][drawFB].data()), kSliceSize);
            out.write(reinterpret_cast<const char *>((*m_meshShadow)[alt][dispFB].data()), kSliceSize);
        }
    }
}

void VulkanVDPRenderer::SetVdp1OpDump(uint32_t frame, std::string path) {
    m_opDump = std::make_unique<vdp1::Vdp1OpDump>();
    m_opDump->frame = frame;
    m_opDumpPath = std::move(path);
    m_opDumpFrames = 0;
    m_opDumpCollecting = false;
}

std::vector<uint8_t> VulkanVDPRenderer::OpDumpSlices() {
    if (!SyncShadow(0xFF, Vdp1SyncKind::Dump, FramePolicy::Sync)) {
        return {};
    }
    std::vector<uint8_t> out(vdp1::kFbramBufferSize);
    const auto put = [&](uint32_t slice, const SpriteFB &fb) {
        std::memcpy(out.data() + size_t{slice} * kSliceSize, fb.data(), kSliceSize);
    };
    for (uint32_t fb = 0; fb < 2; ++fb) {
        put(vdp1::Vdp1MainSlice(fb), m_state.mem1.FBRAM[fb]);
        put(vdp1::Vdp1AltSlice(fb), (*m_altShadow)[fb]);
        for (uint32_t alt = 0; alt < 2; ++alt) {
            put(vdp1::Vdp1MeshSlice(alt, fb), (*m_meshShadow)[alt][fb]);
        }
    }
    return out;
}

void VulkanVDPRenderer::OpDumpFrameEnd() {
    const uint32_t frame = m_opDumpFrames++;
    vdp1::Vdp1OpDump &d = *m_opDump;
    if (!m_gpuVdp1) {
        if (m_opDumpCollecting || frame + 1 == d.frame) {
            Log("VDP1 op dump: abandoned (VDP1 is not on the GPU)");
            m_opDump.reset();
            m_opDumpCollecting = false;
        }
        return;
    }
    if (frame + 1 == d.frame) {
        d.fbramStart = OpDumpSlices();
        if (d.fbramStart.empty()) {
            Log("VDP1 op dump: abandoned (the start framebuffers could not be read back)");
            m_opDump.reset();
            return;
        }
        d.vramStart.assign(m_state.mem1.VRAM.begin(), m_state.mem1.VRAM.end());
        m_opDumpCollecting = true;
        return;
    }
    if (frame != d.frame || !m_opDumpCollecting) {
        return;
    }
    m_opDumpCollecting = false;
    d.fbramEnd = OpDumpSlices();
    if (d.fbramEnd.empty()) {
        Log("VDP1 op dump: abandoned (the end framebuffers could not be read back)");
        m_opDump.reset();
        return;
    }
    d.drawFB = m_state.fbIndex.draw & 1u;
    d.displayFB = m_state.fbIndex.display & 1u;
    d.hres = m_HRes;
    d.vres = m_VRes;
    d.exclusiveMonitor = m_exclusiveMonitor ? 1u : 0u;
    d.deinterlace = m_enhancements.deinterlace ? 1u : 0u;
    d.transparentMeshes = m_enhancements.transparentMeshes ? 1u : 0u;
    d.tvmr = m_state.regs1.ReadTVMR();
    d.fbcr = m_state.regs1.ReadFBCR();
    for (uint32_t i = 0; i < d.vdp2Regs.size(); ++i) {
        d.vdp2Regs[i] = m_state.regs2.Read<true>(i * 2u);
    }
    const uint32_t cramMode = m_state.regs2.vramControl.colorRAMMode;
    for (uint32_t i = 0; i < d.palette.size(); ++i) {
        if (cramMode == 2) {
            d.palette[i] = m_state.mem2.ReadCRAM<uint32>((i * 4u) & 0xFFC) & 0xFFFFFFu; // Color888: R low
        } else {
            const Color555 c{.u16 = m_state.mem2.ReadCRAM<uint16>((i * 2u) & (cramMode == 0 ? 0x7FE : 0xFFE))};
            const auto x8 = [](uint32_t v5) { return (v5 << 3) | (v5 >> 2); };
            d.palette[i] = x8(c.r) | (x8(c.g) << 8) | (x8(c.b) << 16);
        }
    }
    std::string error;
    if (vdp1::WriteVdp1OpDump(m_opDumpPath, d, &error)) {
        Log("VDP1 op dump: frame " + std::to_string(d.frame) + ", " + std::to_string(d.lists.size()) +
            " op lists written to " + m_opDumpPath);
    } else {
        Log("VDP1 op dump: " + error);
    }
    m_opDump.reset();
}

// -----------------------------------------------------------------------------
// VDP2 (front half, then the GPU)

void VulkanVDPRenderer::VDP2WriteVRAM(uint32_t address, uint8_t) {
    m_front.WriteVRAM(address);
}

void VulkanVDPRenderer::VDP2WriteVRAM(uint32_t address, uint16_t) {
    m_front.WriteVRAM(address);
}

void VulkanVDPRenderer::VDP2WriteCRAM(uint32_t address, uint8_t) {
    m_front.WriteCRAM(address);
}

void VulkanVDPRenderer::VDP2WriteCRAM(uint32_t address, uint16_t) {
    m_front.WriteCRAM(address);
}

void VulkanVDPRenderer::VDP2WriteReg(uint32_t address, uint16_t value) {
    m_front.WriteReg(address, value);
}

void VulkanVDPRenderer::VDP2SetResolution(uint32_t h, uint32_t v, bool exclusive) {
    // The software VDP1 erase clamps to the resolution (hybrid section 1.1); the GPU one is given m_VRes.
    m_vdp1->VDP2SetResolution(h, v, exclusive);
    m_front.SetResolution(h, v, exclusive);
    m_HRes = h;
    m_VRes = v;
    m_exclusiveMonitor = exclusive;
    m_resolutionChanged = true;
}

void VulkanVDPRenderer::VDP2SetField(bool) {}

void VulkanVDPRenderer::VDP2LatchTVMD() {}

void VulkanVDPRenderer::VDP2BeginFrame() {
    m_frontSeconds = 0;
    ProbeTimer timer(FrameProbe != nullptr, m_frontSeconds);
    m_front.BeginFrame();
    m_slicesCaptured = false;
    m_altSlicesCaptured = false;
}

void VulkanVDPRenderer::VDP2RenderLine(uint32_t y) {
    ProbeTimer timer(FrameProbe != nullptr, m_frontSeconds);
    m_front.RenderLine(y);
    if (!m_backend || m_gpuVdp1) {
        // Without a device the frame is dropped, so nothing is copied; in Gpu VDP1 mode the sprite pass reads the GPU
        // FBRAM.
        return;
    }
    if (!m_slicesCaptured) {
        // The frame's first line (normally y = 0, after the VBlank swap): the display framebuffer is final for the
        // whole frame (VDP1 draws and the CPU writes into the draw framebuffer; erases happen in blanking).
        CaptureSpriteSlices();
        m_slicesCaptured = true;
    }
    // The sprite shader reads the alt slices on the alt field rows of lines rendered in double density with
    // deinterlacing (outside exclusive monitor modes, which never double rows), deciding per batch from the TVMD those
    // lines rendered with. TVMD can change mid-frame, so the decision is per line too: the first line that can read
    // them captures them (the display framebuffer is the same all frame; the slices upload before any batch).
    if (!m_altSlicesCaptured && m_enhancements.deinterlace && !m_exclusiveMonitor &&
        m_state.regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity) {
        CaptureAltSpriteSlices();
        m_altSlicesCaptured = true;
    }
}

void VulkanVDPRenderer::VDP2EndFrame() {
    {
        ProbeTimer timer(FrameProbe != nullptr, m_frontSeconds);
        m_front.EndFrame();
    }
    if (m_gpuVdp1) {
        ProbeTimer timer(FrameProbe != nullptr, m_vdp1Times.frontSeconds);
        // Segment boundary: the ops so far are replayed before this frame's VDP2 work, later ones after it.
        m_vdp1Front.MarkFrameBoundary();
        QueueVdp1Ops();
        m_vdp1BeforeFrame = m_vdp1Queue.size();
    }
    if (m_opDump) {
        OpDumpFrameEnd();
    }
    if (FrameProbe) {
        if (m_vdp1Backend) {
            m_vdp1Times.gpuSeconds = m_vdp1Backend->TakeGpuSeconds();
        }
        FrameProbe(m_front.Frame().features, m_frontSeconds, m_vdp1Times);
        m_vdp1Times = {};
    }
    if (m_passProfile && PassProfileProbe) {
        const auto micros = m_passProfile->Take();
        std::string line = "GPU passes:";
        char value[32];
        for (uint32_t i = GpuPassProfile::kBoundary + 1; i < GpuPassProfile::kLabelCount; ++i) {
            std::snprintf(value, sizeof(value), " %.1f", micros[i]);
            line += std::string(" ") + GpuPassProfile::Name(static_cast<GpuPassProfile::Label>(i)) + value;
        }
        line += " overflow " + std::to_string(m_passProfile->TakeOverflow());
        PassProfileProbe(line);
    }
    m_slicesCaptured = false;
    m_altSlicesCaptured = false;
    if (!m_backend) {
        // No device: the record is dropped; AttachDevice uploads everything again.
        m_front.ConsumeFrame();
    }
    if (m_resolutionChanged) {
        m_resolutionChanged = false;
        Callbacks.VDP2ResolutionChanged(m_HRes, m_VRes);
    }
    Callbacks.VDP2DrawFinished();
    if (FrameReady) {
        FrameReady(m_HRes, m_VRes);
    }
}

void VulkanVDPRenderer::UpdateEnabledLayers() {
    // Debug layer toggles are not supported by the GPU path: the front half renders every layer.
}

void VulkanVDPRenderer::SyncDisplayErase() {
    if (m_displayEraseSinceCapture) {
        // An erase of the display framebuffer without a swap may still be running on the VDP1 thread.
        m_vdp1->VDP1SyncFB();
        m_displayEraseSinceCapture = false;
    }
}

using Slice = std::span<const uint8_t, vdp2::kSpriteSliceSize>;
static_assert(sizeof(SpriteFB) == vdp2::kSpriteSliceSize);

void VulkanVDPRenderer::CaptureSpriteSlices() {
    SyncDisplayErase();
    const uint8_t display = m_state.fbIndex.display & 1u;
    m_front.AddSpriteSlice(vdp2::kSpriteSliceMain, Slice{m_state.mem1.FBRAM[display]});
    if (m_enhancements.transparentMeshes) {
        m_front.AddSpriteSlice(vdp2::kSpriteSliceMesh0, Slice{m_vdp1->GetMeshFBRAM(false, display)});
    }
}

void VulkanVDPRenderer::CaptureAltSpriteSlices() {
    SyncDisplayErase();
    const uint8_t display = m_state.fbIndex.display & 1u;
    m_front.AddSpriteSlice(vdp2::kSpriteSliceAlt, Slice{m_vdp1->GetAltFBRAM(display)});
    if (m_enhancements.transparentMeshes) {
        m_front.AddSpriteSlice(vdp2::kSpriteSliceMesh1, Slice{m_vdp1->GetMeshFBRAM(true, display)});
    }
}

} // namespace brimir::gpu
