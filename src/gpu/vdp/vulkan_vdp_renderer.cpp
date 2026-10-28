#include "vulkan_vdp_renderer.hpp"

#include "gpu/vdp2/vdp2_bindings.hpp"

#include <algorithm>
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
    , m_meshShadow(std::make_unique<MeshFBRAM>()) {
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
        LeaveGpuVdp1();
    }
    m_backend.reset();
    // The requested scale (capped), else the previous effective one, else 1: the back half tries them in that order,
    // each allocated directly (failures logged by it).
    const uint32_t cap = std::clamp<uint32_t>(maxScale, 1, vdp2::kMaxRenderScale);
    const uint32_t wanted = std::min(m_requestedScale, cap);
    const uint32_t previous = std::min(m_internalScale, cap);
    const bool gpuVdp1 = m_vdp1Mode == Vdp1Mode::Gpu;
    // Gpu VDP1 mode: one more upload context, the sync submits'
    const uint32_t contexts = frameContexts + (gpuVdp1 ? 1u : 0u);
    auto backend = std::make_unique<vdp2::Vdp2VulkanBackend>(device, contexts, wanted, m_scaleAllocationLimit, m_log,
                                                             previous < wanted ? previous : 1);
    if (!backend->Ok()) {
        return;
    }
    m_internalScale = backend->Scale();
    if (m_internalScale != m_requestedScale) {
        Log("VDP2 internal resolution: requested " + std::to_string(m_requestedScale) + "x, using " +
            std::to_string(m_internalScale) + "x");
    }
    m_backend = std::move(backend);
    m_frameContexts = frameContexts;
    // A new back half has none of the state: everything is uploaded with the next frame, and its composite starts
    // cleared (Vdp2VulkanBackend clears it on first use).
    m_front.ConsumeFrame();
    m_front.MarkAllDirty();
    if (gpuVdp1 && !EnterGpuVdp1(device, contexts)) {
        Log("VDP1 on the GPU unavailable; VDP1 stays in software");
    }
}

void VulkanVDPRenderer::DetachDevice() {
    LeaveGpuVdp1();
    m_front.ConsumeFrame();
    m_front.MarkAllDirty();
    m_backend.reset();
}

bool VulkanVDPRenderer::EnsureFrameContexts(uint32_t frameContexts) {
    if (!m_backend) {
        return false;
    }
    // The sync context moves up with the caller's count (its work has always completed: every sync waits).
    const uint32_t extra = m_gpuVdp1 ? 1u : 0u;
    m_backend->EnsureContexts(frameContexts + extra);
    if (m_vdp1Backend) {
        m_vdp1Backend->EnsureContexts(frameContexts + extra);
    }
    m_frameContexts = std::max(m_frameContexts, frameContexts);
    return true;
}

void VulkanVDPRenderer::AbandonDevice() {
    m_front.ConsumeFrame();
    m_front.MarkAllDirty();
    // TODO(Task 6): the GPU FBRAM is lost with the device: VDP1 continues in software from the CPU's (stale) copy.
    (void)m_vdp1Backend.release();
    m_syncPool = VK_NULL_HANDLE;
    m_syncCmd = VK_NULL_HANDLE;
    m_syncFence = VK_NULL_HANDLE;
    m_device = nullptr;
    m_gpuVdp1 = false;
    m_vdp1Queue.clear();
    m_vdp1Recorded.clear();
    m_vdp1BeforeFrame = 0;
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
    if (!HasPendingFrame()) {
        return false;
    }
    const bool recorded = m_backend->Record(m_front.Frame(), context, cmd);
    m_front.ConsumeFrame();
    if (!recorded) {
        // The frame's snapshots never reached the GPU.
        m_front.MarkAllDirty();
    }
    return recorded;
}

bool VulkanVDPRenderer::HasPendingVdp1Work() const {
    return m_gpuVdp1 && (!m_vdp1Queue.empty() || m_vdp1Front.HasPendingOps());
}

bool VulkanVDPRenderer::RecordPendingVdp1Work(uint32_t context, VkCommandBuffer cmd) {
    if (!HasPendingVdp1Work()) {
        m_vdp1Recorded.clear();
        return false;
    }
    bool frameRecorded = false;
    return RecordVdp1Work(context, cmd, FramePolicy::Keep, frameRecorded) && !m_vdp1Recorded.empty();
}

bool VulkanVDPRenderer::TakeFrameRecordedBySync() {
    const bool recorded = m_frameRecordedBySync;
    m_frameRecordedBySync = false;
    return recorded;
}

void VulkanVDPRenderer::RecordedFrameDropped() {
    if (m_backend) {
        m_backend->FrameDropped();
    }
    m_front.MarkAllDirty();
    if (m_vdp1Backend) {
        m_vdp1Backend->FrameDropped();
    }
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
    return FallBackInternalScale(n);
}

bool VulkanVDPRenderer::FallBackInternalScale(uint32_t n) {
    if (n < 1 || n > vdp2::kMaxRenderScale) {
        return false;
    }
    if (m_backend) {
        if (!m_backend->SetScale(n)) {
            return false; // logged by the back half, which keeps its scale
        }
    }
    m_internalScale = n;
    return true;
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

bool VulkanVDPRenderer::EnterGpuVdp1(VulkanDevice &device, uint32_t contexts) {
    auto backend = std::make_unique<vdp1::Vdp1VulkanBackend>(device, contexts, m_backend->SpriteFBBuffer());
    if (!backend->Ok()) {
        return false;
    }
    const auto &vk = device.Table();
    const VkDevice dev = device.Device();
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = device.QueueFamily();
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    bool ok = vk.vkCreateCommandPool(dev, &poolInfo, nullptr, &m_syncPool) == VK_SUCCESS;
    if (ok) {
        allocInfo.commandPool = m_syncPool;
        ok = vk.vkAllocateCommandBuffers(dev, &allocInfo, &m_syncCmd) == VK_SUCCESS &&
             vk.vkCreateFence(dev, &fenceInfo, nullptr, &m_syncFence) == VK_SUCCESS;
    }
    if (!ok) {
        vk.vkDestroyFence(dev, m_syncFence, nullptr);
        vk.vkDestroyCommandPool(dev, m_syncPool, nullptr);
        m_syncPool = VK_NULL_HANDLE;
        m_syncCmd = VK_NULL_HANDLE;
        m_syncFence = VK_NULL_HANDLE;
        return false;
    }
    m_device = &device;
    m_vdp1Backend = std::move(backend);
    m_backend->SetSpriteFBExternal(true);

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
    QueueFbramUpload(0xFF, &m_vdp1->GetAltFBRAM(0), &m_vdp1->GetAltFBRAM(1));
    m_gpuVdp1 = true;
    m_vdp1SyncedOps = Vdp1GpuOpCount();
    m_frameRecordedBySync = false;
    return true;
}

void VulkanVDPRenderer::LeaveGpuVdp1() {
    if (m_gpuVdp1) {
        // Software continues from the GPU's framebuffers: every pending op replayed, main into VDPState, mesh and
        // doubleV into the inner renderer. TODO(Task 6): the alt framebuffers stay the inner renderer's (stale; the
        // same transient as a save state load), until it gets setters for them.
        if (SyncVdp1ToCpu(FramePolicy::Ignore)) {
            auto saved = std::make_unique<savestate::VDPSaveState::VDPRendererSaveState>();
            m_vdp1->SaveState(*saved);
            saved->vdp1State.doubleV = m_vdp1Front.DoubleV() ? 1 : 0;
            saved->vdp1State.meshFBRAM = *m_meshShadow;
            m_vdp1->LoadState(*saved);
        } else {
            Log("VDP1: reading the GPU framebuffers back failed; VDP1 continues in software from older ones");
        }
    }
    DestroyGpuVdp1();
}

void VulkanVDPRenderer::DestroyGpuVdp1() {
    m_vdp1Backend.reset();
    if (m_device != nullptr) {
        const auto &vk = m_device->Table();
        vk.vkDestroyFence(m_device->Device(), m_syncFence, nullptr);
        vk.vkDestroyCommandPool(m_device->Device(), m_syncPool, nullptr); // frees the command buffer
    }
    m_syncPool = VK_NULL_HANDLE;
    m_syncCmd = VK_NULL_HANDLE;
    m_syncFence = VK_NULL_HANDLE;
    m_device = nullptr;
    m_gpuVdp1 = false;
    m_vdp1Queue.clear();
    m_vdp1Recorded.clear();
    m_vdp1BeforeFrame = 0;
    (void)m_vdp1Front.TakeOps();
    if (m_backend) {
        m_backend->SetSpriteFBExternal(false);
    }
}

uint64_t VulkanVDPRenderer::Vdp1GpuOpCount() const {
    // The ops that change FBRAM beyond what the CPU already wrote into VDPState (CPU writes do not count)
    const vdp1::Vdp1FrontStats &stats = m_vdp1Front.Stats();
    return stats.spans + stats.erases;
}

void VulkanVDPRenderer::QueueVdp1Ops() {
    if (m_vdp1Front.HasPendingOps()) {
        m_vdp1Queue.push_back({m_vdp1Front.TakeOps(), {}, 0});
    } else {
        (void)m_vdp1Front.TakeOps(); // op-less frame boundaries
    }
}

void VulkanVDPRenderer::QueueFbramUpload(uint32_t sliceMask, const SpriteFB *alt0, const SpriteFB *alt1) {
    Vdp1Work work;
    work.sliceMask = sliceMask;
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
    if (work.sliceMask != 0) {
        return m_vdp1Backend->RecordFbramUpload(work.fbram, work.sliceMask, context, cmd);
    }
    const vdp1::Vdp1Segment all{0, static_cast<uint32_t>(work.ops.ops.size())};
    return m_vdp1Backend->Record(work.ops, all, context, cmd);
}

bool VulkanVDPRenderer::RecordVdp1Work(uint32_t context, VkCommandBuffer cmd, FramePolicy policy,
                                       bool &frameRecorded) {
    frameRecorded = false;
    m_vdp1Recorded.clear(); // the previous command buffer was submitted (or reported dropped)
    if (!m_vdp1Backend->BeginContext(context)) {
        return false;
    }
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
        m_front.ConsumeFrame();
        if (!frameRecorded) {
            m_front.MarkAllDirty(); // the frame's snapshots never reached the GPU
        }
    }
    if (ok && replayAfter) {
        QueueVdp1Ops();
        replay(m_vdp1Queue.size());
    }
    // What was recorded leaves the queue (RecordedFrameDropped puts it back); what failed stays, in order.
    m_vdp1Recorded.assign(std::make_move_iterator(m_vdp1Queue.begin()),
                          std::make_move_iterator(m_vdp1Queue.begin() + static_cast<std::ptrdiff_t>(done)));
    m_vdp1Queue.erase(m_vdp1Queue.begin(), m_vdp1Queue.begin() + static_cast<std::ptrdiff_t>(done));
    m_vdp1BeforeFrame = m_vdp1BeforeFrame > done ? m_vdp1BeforeFrame - done : 0;
    if (!ok) {
        Log("VDP1: recording the VDP1 ops failed (upload space); they stay pending");
    }
    return ok;
}

bool VulkanVDPRenderer::SyncVdp1ToCpu(FramePolicy policy) {
    if (!m_gpuVdp1 || m_device == nullptr) {
        return false;
    }
    if (Vdp1GpuOpCount() == m_vdp1SyncedOps) {
        // VDPState and the mesh shadow are current: the pending ops are only CPU writes (already in VDPState) and
        // uploads of what the CPU holds.
        return true;
    }
    const auto &vk = m_device->Table();
    const VkDevice dev = m_device->Device();
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk.vkResetCommandPool(dev, m_syncPool, 0) != VK_SUCCESS || vk.vkBeginCommandBuffer(m_syncCmd, &begin) != VK_SUCCESS) {
        return false;
    }
    bool frameRecorded = false;
    const bool recorded = RecordVdp1Work(m_frameContexts, m_syncCmd, policy, frameRecorded);
    const bool readback = recorded && m_vdp1Backend->RecordReadback(m_syncCmd);
    VkResult result = vk.vkEndCommandBuffer(m_syncCmd);
    if (result == VK_SUCCESS && readback) {
        result = vk.vkResetFences(dev, 1, &m_syncFence);
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &m_syncCmd;
            result = m_device->Submit(submit, m_syncFence);
        }
        if (result == VK_SUCCESS) {
            ++m_vdp1SyncSubmits;
            result = vk.vkWaitForFences(dev, 1, &m_syncFence, VK_TRUE, UINT64_MAX);
        } else {
            RecordedFrameDropped();
        }
    } else {
        RecordedFrameDropped();
        return false;
    }
    if (result != VK_SUCCESS) {
        return false;
    }
    m_vdp1Recorded.clear(); // submitted and complete
    m_frameRecordedBySync |= frameRecorded;
    const uint8_t *data = m_vdp1Backend->ReadbackData();
    std::memcpy(m_state.mem1.FBRAM[0].data(), data + size_t{vdp1::Vdp1MainSlice(0)} * kSliceSize, kSliceSize);
    std::memcpy(m_state.mem1.FBRAM[1].data(), data + size_t{vdp1::Vdp1MainSlice(1)} * kSliceSize, kSliceSize);
    for (uint32_t alt = 0; alt < 2; ++alt) {
        for (uint32_t fb = 0; fb < 2; ++fb) {
            std::memcpy((*m_meshShadow)[alt][fb].data(), data + size_t{vdp1::Vdp1MeshSlice(alt, fb)} * kSliceSize,
                        kSliceSize);
        }
    }
    m_vdp1SyncedOps = Vdp1GpuOpCount();
    return true;
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
        // VDPState was reset first (VDP::Reset): a hard reset cleared main FBRAM. Software clears the alt and mesh
        // framebuffers at every reset (sw:94-136): after the ops so far, as uploads of zeros (and main from VDPState).
        m_vdp1Front.Reset(hard);
        QueueVdp1Ops();
        for (auto &alt : *m_meshShadow) {
            for (auto &fb : alt) {
                fb.fill(0);
            }
        }
        QueueFbramUpload(kAltSlices | kMeshSlices | (hard ? kMainSlices : 0u), nullptr, nullptr);
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
    if (m_gpuVdp1 && !SyncVdp1ToCpu(FramePolicy::Sync)) {
        Log("VDP1: reading the GPU framebuffers back for a save state failed; the state holds older ones");
    }
}

void VulkanVDPRenderer::PostLoadStateSync() {
    m_vdp1->PostLoadStateSync();
    m_front.PostLoadStateSync();
    if (m_gpuVdp1) {
        // The ops before the load happened (their alt writes stay, as software's), then the loaded framebuffers.
        QueueVdp1Ops();
        m_vdp1Front.PostLoadStateSync();
        if (m_stateLoaded) {
            QueueFbramUpload(kMainSlices | kMeshSlices, nullptr, nullptr);
            m_vdp1SyncedOps = Vdp1GpuOpCount();
        }
    }
    m_stateLoaded = false;
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
        *m_meshShadow = state.vdp1State.meshFBRAM;
        m_stateLoaded = true;
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
        // TODO(Task 6): the CPU shadow (one sync submit per read burst, readbacks with every submit).
        if (!SyncVdp1ToCpu(FramePolicy::Sync)) {
            Log("VDP1: reading the GPU framebuffers back failed; the CPU reads older data");
        }
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
        m_vdp1Front.WriteFB8(address, value);
        return;
    }
    m_vdp1->VDP1WriteFB(address, value);
}

void VulkanVDPRenderer::VDP1WriteFB(uint32_t address, uint16_t value) {
    if (m_gpuVdp1) {
        m_vdp1Front.WriteFB16(address, value);
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
        m_vdp1Front.BeginFrame();
        return;
    }
    m_vdp1->VDP1BeginFrame();
}

void VulkanVDPRenderer::VDP1ExecuteCommand(uint32_t cmdAddress, VDP1Command::Control control) {
    if (m_gpuVdp1) {
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
    // Gpu mode: the inner renderer's framebuffers as of the last switch (debug only)
    m_vdp1->DumpExtraVDP1Framebuffers(out);
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
        // Segment boundary: the ops so far are replayed before this frame's VDP2 work, later ones after it.
        m_vdp1Front.MarkFrameBoundary();
        QueueVdp1Ops();
        m_vdp1BeforeFrame = m_vdp1Queue.size();
    }
    if (FrameProbe) {
        FrameProbe(m_front.Frame().features, m_frontSeconds);
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
