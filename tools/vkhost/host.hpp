// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Loads a libretro core dynamically (LoadLibrary/dlopen, like RetroArch), answers its environment calls and
// hands every displayed frame to a FrameSink. See tools/README.md.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <libretro.h>

#include "vulkan_provider.hpp"

namespace vkhost {

// One displayed frame as XRGB8888 (0x00RRGGBB, tightly packed: pitch == w * 4).
struct HostFrame {
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint32_t> xrgb;
    // Internal resolution of the frame as the core last logged it ("Internal resolution: frames at Nx"); 0 when the
    // core has not said (the software renderer without a Vulkan output).
    uint32_t scale = 0;
};

// Receives the frames the core displays while recording is on (after the warmup).
class FrameSink {
public:
    virtual ~FrameSink() = default;
    virtual void OnFrame(HostFrame&& frame) = 0;
};

struct HostConfig {
    std::string corePath;  // libretro core library; empty: brimir_libretro next to this executable
    std::string biosPath;  // copied into the system directory under its own file name
    std::string gamePath;  // empty: no content (the BIOS runs on its own)
    std::string systemDir; // empty: a fresh temporary directory
    std::string dumpDir;   // empty: no PPM dump (handled by the CLI layer)
    std::vector<std::pair<std::string, std::string>> options; // --option key=value overrides
    int warmup = 120;
    int frames = 300;
    bool verbose = false; // also log RETRO_LOG_DEBUG and the environment calls

    // Hardware rendering (Vulkan host, see vulkan_provider.hpp).
    bool vulkan = false;   // --vulkan: offer a Vulkan context (and select brimir_renderer=vulkan)
    bool refuseHw = false; // --refuse-hw: SET_HW_RENDER returns false (a frontend on a gl/d3d driver)
    int resetAt = -1;      // --reset-at F: context_destroy + context_reset (a driver reinit) before frame F
    int resetInRun = -1;   // --reset-in-run F: the same from inside frame F's retro_run (as SET_SYSTEM_AV_INFO)
    int resetInPresent = -1; // --reset-in-present F: the same at frame F's GET_AUDIO_VIDEO_ENABLE (after the
                             // emulated frame, before the core records and presents it)
    int hideEvery = 0;       // --hide-every N: before every Nth frame, one extra frame with video disabled
    int stateRoundtrip = 0;  // --state-roundtrip N: retro_serialize + retro_unserialize after every Nth frame
    int saveStateAt = -1;    // --save-state-at F <file>: retro_serialize after frame F into saveStatePath
    std::string saveStatePath;
    std::string loadStatePath; // --load-state <file>: retro_unserialize before the first frame
    bool runahead = false; // --runahead: run-ahead simulation (hidden frames, a second instance without context)
    bool preferGl = false; // --preferred-hw gl: GET_PREFERRED_HW_RENDER reports GL (SET_HW_RENDER still accepted)
    uint32_t fakeApi = 0;  // --fake-api X.Y: the core is told the GPU supports only this Vulkan version
    int growMaskAt = -1;   // --grow-mask F: sync mask 0b1 until frame F, then 0b111
    bool noValidation = false;      // --no-validation: do not load the validation layer (timing runs)
    bool failComputeOnGrow = false; // --fail-compute-on-grow: the core's pipeline creation fails after the growth
    bool failComputeOnReset = false; // --fail-compute-on-reset: ... from the second context on (a reinit)
    bool probe = false; // --probe: collect the core's per-frame VDP2 probe lines (BRIMIR_VDP2_PROBE) while recording
    // --set-option-at F key=value: the option changes before frame F (counting warmup frames from 0, as --reset-at),
    // reported to the core through GET_VARIABLE_UPDATE (a live change, as RetroArch's Quick Menu).
    struct OptionChange {
        int frame = 0;
        std::string key, value;
    };
    std::vector<OptionChange> optionChanges;
    // --fail-alloc-above MiB: with --vulkan, the core's vkAllocateMemory fails (VK_ERROR_OUT_OF_DEVICE_MEMORY) for
    // allocations above this size (0: never).
    uint64_t failAllocAboveMiB = 0;
    // --fail-output-images-above N: with --vulkan, the core's output images (set_image) wider than 704 * N fail.
    uint32_t failOutputImagesAbove = 0;
    // --defer-image-read N: with --vulkan, each set_image image is read N displayed frames later (1 or 2: before the
    // core's next wait_sync_index for its sync index, the latest a frontend may read it), as a threaded frontend does.
    int deferImageRead = 0;
};

// One VDP2 frame's "VDP2 probe:" line from the Vulkan VDP renderer (the front half's Vdp2FeatureCounters).
struct ProbeFrame {
    uint32_t batches = 0;
    uint32_t arenaBytes = 0;
    uint32_t copyBytes = 0;
    uint32_t mosaicLines = 0;
    uint32_t vcellLines = 0;
    uint32_t charPatDelayLines = 0;
    uint32_t ddLineScrollLines = 0;
    uint32_t extendedCCLines = 0;
    uint32_t shadowLines = 0;
    uint32_t meshSlices = 0;
    uint32_t cramModeMask = 0;
    double frontMicroseconds = 0;
};

// What a run did, for the CLI summary and the checks.
struct RunReport {
    uint64_t recordedFrames = 0;
    uint64_t recordedHardwareFrames = 0; // read back from set_image images
    uint64_t contextResets = 0;          // context_reset calls
    uint64_t coreSubmits = 0;            // the core's vkQueueSubmit calls
    uint64_t lockViolations = 0;
    uint64_t hiddenGpuCalls = 0; // presentation calls or hardware frames in frames with video disabled
    uint64_t hiddenSubmits = 0;  // command buffer submits in frames with video disabled (the core's VDP2 work)
    uint64_t maxRunSubmits = 0;  // most command buffer submits in one retro_run (must stay at most 1)
    uint64_t unreadableFrames = 0;
    bool validation = false;
    uint32_t validationErrors = 0;
    uint64_t secondaryFrames = 0;          // run-ahead: frames the second instance ran
    uint64_t secondaryHardwareFrames = 0;  // run-ahead: must stay 0
    bool secondaryHadContext = false;      // run-ahead: must stay false
    double recordSeconds = 0;   // wall time of the recorded frames' retro_run calls (sink time included)
    double sinkSeconds = 0;     // of which in FrameSink::OnFrame (hash, PPM)
    double readbackSeconds = 0; // of which in the host's set_image readback (Vulkan)
    uint64_t systemAvInfoCalls = 0; // SET_SYSTEM_AV_INFO (a frontend may reinit its video driver)
    uint64_t geometryCalls = 0;     // SET_GEOMETRY
    std::vector<ProbeFrame> probeFrames; // --probe: the VDP2 frames that ended while recording
    std::vector<std::string> problems; // anything that fails the run
};

// Loads a core and drives it. At most two instances at a time (the run-ahead simulation's second one); each
// needs its own copy of the library, as RetroArch makes for run-ahead.
class LibretroHost {
public:
    LibretroHost();
    ~LibretroHost();
    LibretroHost(const LibretroHost&) = delete;
    LibretroHost& operator=(const LibretroHost&) = delete;

    // Loads the library, retro_set_environment/retro_init and the content, then (Vulkan) creates the context and
    // calls context_reset. Returns false (and logs why) on failure. Throws VulkanUnavailable for --vulkan without
    // a usable device.
    bool Load(const HostConfig& config, const std::string& systemDir, const std::string& saveDir);
    // context_destroy, retro_unload_game, destroy_device, retro_deinit, unloads the library. Safe to call twice.
    void Unload();

    // Runs one frame. Frames the core displays go to `sink` while it is non-null.
    bool RunFrame(FrameSink* sink);

    // A video driver reinit: context_destroy, a new device through the negotiation interface, context_reset.
    bool ResetContext();

    // retro_serialize / retro_unserialize.
    bool Serialize(std::vector<uint8_t>& state);
    bool Unserialize(const std::vector<uint8_t>& state);

    // Number of video_refresh calls that delivered pixels / no new frame (NULL data) / a frame we cannot read.
    uint64_t DisplayedFrames() const { return m_displayed; }
    uint64_t DuplicateFrames() const { return m_duplicate; }
    uint64_t UnreadableFrames() const { return m_unreadable; }
    uint64_t HardwareFrames() const { return m_hardwareFrames; }
    double ReadbackSeconds() const { return m_readbackSeconds; }
    // --fail-alloc-above: the core's allocations refused (complete after Unload).
    uint64_t FailedAllocations() const { return m_vulkan ? m_vulkan->FailedAllocations() : m_failedAllocations; }
    bool HadContext() const { return m_contextResets > 0; }

    // Adds this instance's counters and problems (Vulkan ones too; complete after Unload).
    void FillReport(RunReport& report) const;

    // What the core last reported through SET_GEOMETRY / SET_SYSTEM_AV_INFO.
    const retro_system_av_info& AvInfo() const { return m_avInfo; }
    const retro_game_geometry& Geometry() const { return m_geometry; }

    // Value of GET_AUDIO_VIDEO_ENABLE (RETRO_AV_ENABLE_* flags); the core sees it on its next query.
    void SetAvEnable(int flags) { m_avEnable = flags; }

    // A "VDP2 probe:" log line (--probe); kept only while recording. False if it does not parse.
    bool ProbeLine(const char* text);
    // A core log line; follows "Internal resolution: frames at Nx" (HostFrame::scale).
    void CoreLogLine(const char* text);

    // --set-option-at: a live option change, reported by the next GET_VARIABLE_UPDATE.
    void SetOption(const std::string& key, const std::string& value);

private:
    struct Api;

    template <int Slot>
    static bool RETRO_CALLCONV EnvironmentThunk(unsigned cmd, void* data);
    template <int Slot>
    static void RETRO_CALLCONV VideoThunk(const void* data, unsigned width, unsigned height, size_t pitch);
    static void RETRO_CALLCONV AudioSampleThunk(int16_t left, int16_t right);
    static size_t RETRO_CALLCONV AudioBatchThunk(const int16_t* data, size_t frames);
    static void RETRO_CALLCONV InputPollThunk();
    static int16_t RETRO_CALLCONV InputStateThunk(unsigned port, unsigned device, unsigned index, unsigned id);

    bool Environment(unsigned cmd, void* data);
    void Video(const void* data, unsigned width, unsigned height, size_t pitch);
    void LogUnsupportedOnce(unsigned cmd);
    void StoreOptionDefaults(const retro_core_options_v2& options);
    bool StartContext();
    void Problem(const std::string& text);
    // --defer-image-read: reads (and delivers) the oldest deferred frames until at most `keep` are left.
    void ProcessDeferred(size_t keep);
    void DeliverFrame(HostFrame&& frame, FrameSink* sink, bool hardware);

    std::unique_ptr<Api> m_api;
    int m_slot = -1;
    bool m_coreInitialized = false;
    bool m_contentLoaded = false;

    // Vulkan host. The provider calls into the core (destroy_device), so it goes after retro_deinit and before
    // the library is unloaded.
    std::unique_ptr<VulkanProvider> m_vulkan;
    bool m_refuseHw = false;
    bool m_preferGl = false;
    int m_growMaskAt = -1;
    HostFrame m_lastFrame; // repeated for video_refresh(NULL)
    struct DeferredFrame {
        VulkanProvider::DeferredImage image;
        bool dupe = false;  // video_refresh(NULL): repeat the frame before it
        FrameSink* sink = nullptr; // where it goes (null during warmup and hidden frames)
        uint32_t scale = 0;
    };
    std::vector<DeferredFrame> m_deferred; // oldest first
    int m_deferImageRead = 0;
    int m_resetInRun = -1;
    bool m_resetInRunPending = false;
    int m_resetInPresent = -1;
    bool m_resetInPresentPending = false;
    int m_frameIndex = 0;
    bool m_hwAccepted = false;
    retro_hw_render_callback m_hwRender{};
    const retro_hw_render_context_negotiation_interface* m_negotiation = nullptr;
    bool m_contextLive = false;
    uint64_t m_contextResets = 0;
    uint64_t m_hardwareFrames = 0;
    uint64_t m_recordedFrames = 0;
    uint64_t m_recordedHardware = 0;
    uint64_t m_hiddenFrames = 0;
    // Copied from the provider before it is destroyed.
    uint64_t m_coreSubmits = 0;
    uint64_t m_lockViolations = 0;
    uint64_t m_hiddenGpuCalls = 0;
    uint64_t m_hiddenSubmits = 0;
    uint64_t m_maxRunSubmits = 0;
    uint64_t m_failedAllocations = 0;
    bool m_validation = false;
    uint32_t m_validationErrors = 0;
    std::vector<std::string> m_problems;

    std::string m_systemDir;
    std::string m_saveDir;
    std::map<std::string, std::string> m_defaults;  // from SET_CORE_OPTIONS_V2
    std::map<std::string, std::string> m_overrides; // --option, --set-option-at
    bool m_optionsUpdated = false;                  // GET_VARIABLE_UPDATE reports true once
    uint32_t m_frameScale = 0;                      // "Internal resolution: frames at Nx"
    uint64_t m_systemAvInfoCalls = 0;
    uint64_t m_geometryCalls = 0;
    std::set<unsigned> m_unsupportedLogged;
    bool m_verbose = false;

    retro_pixel_format m_pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_system_av_info m_avInfo{};
    retro_game_geometry m_geometry{};
    int m_avEnable = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;

    FrameSink* m_sink = nullptr;
    uint64_t m_displayed = 0;
    uint64_t m_duplicate = 0;
    uint64_t m_unreadable = 0;
    double m_readbackSeconds = 0;
    std::vector<ProbeFrame> m_probeFrames;
};

// Loads the core per `config`, runs `warmup` unrecorded frames then `frames` recorded ones, and returns the recorded
// frames (one HostFrame per displayed frame). Throws std::runtime_error on setup failure (VulkanUnavailable when
// --vulkan has no device). `report` (optional) receives the run's counters and problems.
std::vector<HostFrame> RunCore(const HostConfig& config, RunReport* report = nullptr);
// Same, delivering the frames to `sink` instead of keeping them.
RunReport RunCore(const HostConfig& config, FrameSink& sink);

} // namespace vkhost
