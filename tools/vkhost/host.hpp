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
    bool runahead = false; // --runahead: run-ahead simulation (hidden frames, a second instance without context)
    bool preferGl = false; // --preferred-hw gl: GET_PREFERRED_HW_RENDER reports GL (SET_HW_RENDER still accepted)
    uint32_t fakeApi = 0;  // --fake-api X.Y: the core is told the GPU supports only this Vulkan version
    int growMaskAt = -1;   // --grow-mask F: sync mask 0b1 until frame F, then 0b111
    bool failComputeOnGrow = false; // --fail-compute-on-grow: the core's pipeline creation fails after the growth
};

// What a run did, for the CLI summary and the checks.
struct RunReport {
    uint64_t recordedFrames = 0;
    uint64_t recordedHardwareFrames = 0; // read back from set_image images
    uint64_t contextResets = 0;          // context_reset calls
    uint64_t coreSubmits = 0;            // the core's vkQueueSubmit calls
    uint64_t lockViolations = 0;
    uint64_t hiddenGpuCalls = 0; // GPU work or hardware frames in frames with video disabled
    uint64_t unreadableFrames = 0;
    bool validation = false;
    uint32_t validationErrors = 0;
    uint64_t secondaryFrames = 0;          // run-ahead: frames the second instance ran
    uint64_t secondaryHardwareFrames = 0;  // run-ahead: must stay 0
    bool secondaryHadContext = false;      // run-ahead: must stay false
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
    bool HadContext() const { return m_contextResets > 0; }

    // Adds this instance's counters and problems (Vulkan ones too; complete after Unload).
    void FillReport(RunReport& report) const;

    // What the core last reported through SET_GEOMETRY / SET_SYSTEM_AV_INFO.
    const retro_system_av_info& AvInfo() const { return m_avInfo; }
    const retro_game_geometry& Geometry() const { return m_geometry; }

    // Value of GET_AUDIO_VIDEO_ENABLE (RETRO_AV_ENABLE_* flags); the core sees it on its next query.
    void SetAvEnable(int flags) { m_avEnable = flags; }

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
    int m_resetInRun = -1;
    bool m_resetInRunPending = false;
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
    bool m_validation = false;
    uint32_t m_validationErrors = 0;
    std::vector<std::string> m_problems;

    std::string m_systemDir;
    std::string m_saveDir;
    std::map<std::string, std::string> m_defaults;  // from SET_CORE_OPTIONS_V2
    std::map<std::string, std::string> m_overrides; // --option
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
};

// Loads the core per `config`, runs `warmup` unrecorded frames then `frames` recorded ones, and returns the recorded
// frames (one HostFrame per displayed frame). Throws std::runtime_error on setup failure (VulkanUnavailable when
// --vulkan has no device). `report` (optional) receives the run's counters and problems.
std::vector<HostFrame> RunCore(const HostConfig& config, RunReport* report = nullptr);
// Same, delivering the frames to `sink` instead of keeping them.
RunReport RunCore(const HostConfig& config, FrameSink& sink);

} // namespace vkhost
