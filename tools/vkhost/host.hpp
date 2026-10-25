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
};

// Loads a core and drives it. One instance at a time (libretro callbacks carry no user data).
class LibretroHost {
public:
    LibretroHost();
    ~LibretroHost();
    LibretroHost(const LibretroHost&) = delete;
    LibretroHost& operator=(const LibretroHost&) = delete;

    // Loads the library, retro_set_environment/retro_init and the content. Returns false (and logs why) on failure.
    bool Load(const HostConfig& config, const std::string& systemDir, const std::string& saveDir);
    // retro_unload_game, retro_deinit, unloads the library. Safe to call twice.
    void Unload();

    // Runs one frame. Frames the core displays go to `sink` while it is non-null.
    bool RunFrame(FrameSink* sink);

    // Number of video_refresh calls that delivered pixels / no new frame (NULL data) / a frame we cannot read.
    uint64_t DisplayedFrames() const { return m_displayed; }
    uint64_t DuplicateFrames() const { return m_duplicate; }
    uint64_t UnreadableFrames() const { return m_unreadable; }

    // What the core last reported through SET_GEOMETRY / SET_SYSTEM_AV_INFO.
    const retro_system_av_info& AvInfo() const { return m_avInfo; }
    const retro_game_geometry& Geometry() const { return m_geometry; }

    // Value of GET_AUDIO_VIDEO_ENABLE (RETRO_AV_ENABLE_* flags); the core sees it on its next query.
    void SetAvEnable(int flags) { m_avEnable = flags; }

private:
    struct Api;

    static bool RETRO_CALLCONV EnvironmentThunk(unsigned cmd, void* data);
    static void RETRO_CALLCONV VideoThunk(const void* data, unsigned width, unsigned height, size_t pitch);
    static void RETRO_CALLCONV AudioSampleThunk(int16_t left, int16_t right);
    static size_t RETRO_CALLCONV AudioBatchThunk(const int16_t* data, size_t frames);
    static void RETRO_CALLCONV InputPollThunk();
    static int16_t RETRO_CALLCONV InputStateThunk(unsigned port, unsigned device, unsigned index, unsigned id);

    bool Environment(unsigned cmd, void* data);
    void Video(const void* data, unsigned width, unsigned height, size_t pitch);
    void LogUnsupportedOnce(unsigned cmd);
    void StoreOptionDefaults(const retro_core_options_v2& options);

    std::unique_ptr<Api> m_api;
    bool m_coreInitialized = false;
    bool m_contentLoaded = false;

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
// frames (one HostFrame per displayed frame). Throws std::runtime_error on setup failure.
std::vector<HostFrame> RunCore(const HostConfig& config);
// Same, delivering the frames to `sink` instead of keeping them.
void RunCore(const HostConfig& config, FrameSink& sink);

} // namespace vkhost
