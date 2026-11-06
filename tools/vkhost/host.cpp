// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "host.hpp"

#include <libretro_vulkan.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace vkhost {

namespace {

// Command numbers without the EXPERIMENTAL/PRIVATE flag bits, for the dispatch switch.
constexpr unsigned kEnv(unsigned cmd) {
    return cmd & ~(RETRO_ENVIRONMENT_EXPERIMENTAL | RETRO_ENVIRONMENT_PRIVATE);
}

// Libretro callbacks carry no user data: one set of thunks per instance slot.
constexpr int kMaxHosts = 2;
LibretroHost* g_hosts[kMaxHosts] = {};

// --input-pattern: the seed (0: no input) and the frame whose input the instances read.
uint32_t g_inputSeed = 0;
int g_inputFrame = 0;

uint64_t SplitMix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Scripted input: held for 6 frames at a time (mouse movement changes every frame), so games see presses, holds and
// releases. A function of its arguments only.
int16_t ScriptedInput(unsigned port, unsigned device, unsigned index, unsigned id) {
    const uint64_t period = static_cast<uint64_t>(g_inputFrame / 6);
    const auto draw = [&](uint64_t salt, uint64_t time, unsigned which) {
        return SplitMix(SplitMix(SplitMix(g_inputSeed ^ (time << 20)) ^ (uint64_t{port} << 8) ^ (uint64_t{device} << 16) ^
                                 (uint64_t{index} << 24) ^ (uint64_t{which} << 32)) ^
                        salt);
    };
    switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_JOYPAD: {
        // About one button in four pressed (two draws ANDed); one mask for the bitmask and the per-button queries
        const uint64_t mask = draw(1, period, 0) & draw(2, period, 0) & 0xFFFF;
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            return static_cast<int16_t>(mask);
        }
        return static_cast<int16_t>((mask >> id) & 1);
    }
    case RETRO_DEVICE_ANALOG:
        if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
            return static_cast<int16_t>(draw(3, period, id) & 0x7FFF);
        }
        return static_cast<int16_t>(draw(3, period, id) & 0xFFFF);
    case RETRO_DEVICE_MOUSE:
        if (id == RETRO_DEVICE_ID_MOUSE_X || id == RETRO_DEVICE_ID_MOUSE_Y) {
            return static_cast<int16_t>(static_cast<int>(draw(4, static_cast<uint64_t>(g_inputFrame), id) % 41) - 20);
        }
        return static_cast<int16_t>(draw(5, period, id) % 3 == 0);
    case RETRO_DEVICE_LIGHTGUN:
        if (id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X || id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y) {
            return static_cast<int16_t>(static_cast<int32_t>(draw(6, period, id) % 0xFFFF) - 0x7FFF);
        }
        if (id == RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) {
            return static_cast<int16_t>(draw(7, period, id) % 8 == 0);
        }
        return static_cast<int16_t>(draw(8, period, id) % 3 == 0);
    default: return 0;
    }
}
bool g_verbose = false;

// Shared library handle, loaded the way a frontend does.
class DynLib {
public:
    DynLib() = default;
    ~DynLib() { Close(); }
    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;

    bool Open(const std::string& path, std::string& error) {
#ifdef _WIN32
        m_handle = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m_handle == nullptr) {
            error = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
        }
#else
        m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (m_handle == nullptr) {
            const char* message = dlerror();
            error = message != nullptr ? message : "dlopen failed";
        }
#endif
        return m_handle != nullptr;
    }

    void Close() {
        if (m_handle == nullptr) {
            return;
        }
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(m_handle));
#else
        dlclose(m_handle);
#endif
        m_handle = nullptr;
    }

    void* Symbol(const char* name) const {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
        return dlsym(m_handle, name);
#endif
    }

private:
    void* m_handle = nullptr;
};

template <int Slot>
void RETRO_CALLCONV LogCallback(enum retro_log_level level, const char* fmt, ...) {
    char text[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    // --probe lines are collected, not printed.
    if (std::strstr(text, "VDP2 probe: ") != nullptr && g_hosts[Slot] != nullptr && g_hosts[Slot]->ProbeLine(text)) {
        return;
    }
    if (std::strstr(text, "VDP1 probe: ") != nullptr && g_hosts[Slot] != nullptr &&
        g_hosts[Slot]->Vdp1ProbeLine(text)) {
        return;
    }
    if (std::strstr(text, "GPU passes: ") != nullptr && g_hosts[Slot] != nullptr && g_hosts[Slot]->PassLine(text)) {
        return;
    }
    if (g_hosts[Slot] != nullptr) {
        g_hosts[Slot]->CoreLogLine(text);
    }
    if (level == RETRO_LOG_DEBUG && !g_verbose) {
        return;
    }
    static const char* const names[] = {"debug", "info", "warn", "error"};
    const unsigned index = static_cast<unsigned>(level);
    std::fprintf(stderr, Slot == 0 ? "[core %s] %s" : "[core#2 %s] %s", index < 4 ? names[index] : "?", text);
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

struct LibretroHost::Api {
    DynLib lib;
    void (*set_environment)(retro_environment_t) = nullptr;
    void (*set_video_refresh)(retro_video_refresh_t) = nullptr;
    void (*set_audio_sample)(retro_audio_sample_t) = nullptr;
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void (*set_input_poll)(retro_input_poll_t) = nullptr;
    void (*set_input_state)(retro_input_state_t) = nullptr;
    void (*init)() = nullptr;
    void (*deinit)() = nullptr;
    unsigned (*api_version)() = nullptr;
    void (*get_system_info)(retro_system_info*) = nullptr;
    void (*get_system_av_info)(retro_system_av_info*) = nullptr;
    bool (*load_game)(const retro_game_info*) = nullptr;
    void (*unload_game)() = nullptr;
    void (*run)() = nullptr;
    size_t (*serialize_size)() = nullptr;
    bool (*serialize)(void*, size_t) = nullptr;
    bool (*unserialize)(const void*, size_t) = nullptr;
    void* (*get_memory_data)(unsigned) = nullptr;
    size_t (*get_memory_size)(unsigned) = nullptr;
    void (*cheat_reset)() = nullptr;
    void (*cheat_set)(unsigned, bool, const char*) = nullptr;
    void (*set_controller_port_device)(unsigned, unsigned) = nullptr;
};

LibretroHost::LibretroHost() = default;

LibretroHost::~LibretroHost() {
    Unload();
}

bool LibretroHost::Load(const HostConfig& config, const std::string& systemDir, const std::string& saveDir) {
    if (m_slot >= 0) {
        std::fprintf(stderr, "vkhost: this host already has a core loaded\n");
        return false;
    }
    int slot = 0;
    while (slot < kMaxHosts && g_hosts[slot] != nullptr) {
        ++slot;
    }
    if (slot == kMaxHosts) {
        std::fprintf(stderr, "vkhost: at most %d cores can be loaded at a time\n", kMaxHosts);
        return false;
    }
    m_systemDir = systemDir;
    m_saveDir = saveDir;
    m_verbose = config.verbose;
    g_verbose = config.verbose;
    m_overrides.clear();
    for (const auto& [key, value] : config.options) {
        m_overrides[key] = value;
    }
    m_refuseHw = config.refuseHw;
    m_preferGl = config.preferGl;
    m_growMaskAt = config.growMaskAt;
    m_resetInRun = config.resetInRun;
    m_resetInPresent = config.resetInPresent;
    m_frameIndex = 0;
    m_probeFrames.clear();
    m_vdp1ProbeFrames.clear();
    m_passNames.clear();
    m_passFrames.clear();
    if (config.probePasses) {
        // Read by the core when it installs the Vulkan VDP renderer
#ifdef _WIN32
        _putenv_s("BRIMIR_GPU_PASS_PROFILE", "1");
#else
        setenv("BRIMIR_GPU_PASS_PROFILE", "1", 1);
#endif
    }
    if (config.probe) {
        // The core reads BRIMIR_VDP2_PROBE when it installs the Vulkan VDP renderer, BRIMIR_VDP1_PROBE on its first
        // retro_run (only cores built with BRIMIR_VDP1_PROBE=ON count VDP1 work).
#ifdef _WIN32
        _putenv_s("BRIMIR_VDP2_PROBE", "1");
        _putenv_s("BRIMIR_VDP1_PROBE", "1");
#else
        setenv("BRIMIR_VDP2_PROBE", "1", 1);
        setenv("BRIMIR_VDP1_PROBE", "1", 1);
#endif
    }
    if (config.gpuVdp1 || config.softwareVdp1) {
        // The core reads BRIMIR_VDP1_GPU when it first attaches the Vulkan VDP renderer (VDP1 on the GPU unless 0).
        const char* value = config.softwareVdp1 ? "0" : "1";
#ifdef _WIN32
        _putenv_s("BRIMIR_VDP1_GPU", value);
#else
        setenv("BRIMIR_VDP1_GPU", value, 1);
#endif
    }
    if (config.vulkan && !config.refuseHw) {
        // Like a frontend's video driver, the instance and device exist before the core is loaded.
        m_vulkan = std::make_unique<VulkanProvider>(config.fakeApi, !config.noValidation);
        if (config.growMaskAt >= 0) {
            m_vulkan->SetSyncSlots(1);
        }
        m_vulkan->SetFailComputeOnGrow(config.failComputeOnGrow);
        m_vulkan->SetFailComputeOnReset(config.failComputeOnReset);
        m_vulkan->SetFailAllocAbove(config.failAllocAboveMiB << 20);
        m_vulkan->SetFailAllocRuns(config.failAllocFirst, config.failAllocEnd);
        m_vulkan->SetFailAllocTotalAbove(config.failAllocTotalAboveMiB << 20);
        if (config.failOutputImagesOnGrow) {
            m_vulkan->SetFailOutputImagesOnGrow(config.failOutputImagesAbove);
        } else {
            m_vulkan->SetFailOutputImagesAbove(config.failOutputImagesAbove);
        }
        m_vulkan->SetFailSubmits(config.failSubmitAt, config.failSubmits);
        m_vulkan->SetFailOutputImagesRuns(config.failOutputImagesFirst, config.failOutputImagesEnd);
    }
    m_haveCaptured = false;
    m_deferImageRead = m_vulkan ? config.deferImageRead : 0;
    m_deferred.clear();
    m_frameScale = 0;
    m_optionsUpdated = false;
    m_systemAvInfoCalls = 0;
    m_geometryCalls = 0;

    auto api = std::make_unique<Api>();
    std::string error;
    if (!api->lib.Open(config.corePath, error)) {
        std::fprintf(stderr, "vkhost: cannot load core %s: %s\n", config.corePath.c_str(), error.c_str());
        return false;
    }
    bool missing = false;
    const auto resolve = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(api->lib.Symbol(name));
        if (fn == nullptr) {
            std::fprintf(stderr, "vkhost: core does not export %s\n", name);
            missing = true;
        }
    };
    resolve(api->set_environment, "retro_set_environment");
    resolve(api->set_video_refresh, "retro_set_video_refresh");
    resolve(api->set_audio_sample, "retro_set_audio_sample");
    resolve(api->set_audio_sample_batch, "retro_set_audio_sample_batch");
    resolve(api->set_input_poll, "retro_set_input_poll");
    resolve(api->set_input_state, "retro_set_input_state");
    resolve(api->init, "retro_init");
    resolve(api->deinit, "retro_deinit");
    resolve(api->api_version, "retro_api_version");
    resolve(api->get_system_info, "retro_get_system_info");
    resolve(api->get_system_av_info, "retro_get_system_av_info");
    resolve(api->load_game, "retro_load_game");
    resolve(api->unload_game, "retro_unload_game");
    resolve(api->run, "retro_run");
    resolve(api->serialize_size, "retro_serialize_size");
    resolve(api->serialize, "retro_serialize");
    resolve(api->unserialize, "retro_unserialize");
    resolve(api->get_memory_data, "retro_get_memory_data");
    resolve(api->get_memory_size, "retro_get_memory_size");
    resolve(api->cheat_reset, "retro_cheat_reset");
    resolve(api->cheat_set, "retro_cheat_set");
    resolve(api->set_controller_port_device, "retro_set_controller_port_device");
    if (missing) {
        return false;
    }
    if (api->api_version() != RETRO_API_VERSION) {
        std::fprintf(stderr, "vkhost: core reports libretro API version %u, expected %u\n", api->api_version(),
                     static_cast<unsigned>(RETRO_API_VERSION));
        return false;
    }

    m_api = std::move(api);
    m_slot = slot;
    g_hosts[slot] = this;

    // RetroArch order: environment, init, the other callbacks, then the content.
    m_api->set_environment(slot == 0 ? &LibretroHost::EnvironmentThunk<0> : &LibretroHost::EnvironmentThunk<1>);
    m_api->init();
    m_coreInitialized = true;
    m_api->set_video_refresh(slot == 0 ? &LibretroHost::VideoThunk<0> : &LibretroHost::VideoThunk<1>);
    m_api->set_audio_sample(&LibretroHost::AudioSampleThunk);
    m_api->set_audio_sample_batch(&LibretroHost::AudioBatchThunk);
    m_api->set_input_poll(&LibretroHost::InputPollThunk);
    m_api->set_input_state(&LibretroHost::InputStateThunk);

    retro_system_info info{};
    m_api->get_system_info(&info);
    std::fprintf(stderr, "vkhost: core %s %s\n", info.library_name != nullptr ? info.library_name : "?",
                 info.library_version != nullptr ? info.library_version : "?");

    retro_game_info game{};
    std::vector<uint8_t> gameData;
    const bool hasGame = !config.gamePath.empty();
    if (hasGame) {
        game.path = config.gamePath.c_str();
        if (!info.need_fullpath) {
            if (!ReadWholeFile(config.gamePath, gameData)) {
                std::fprintf(stderr, "vkhost: cannot read %s\n", config.gamePath.c_str());
                return false;
            }
            game.data = gameData.data();
            game.size = gameData.size();
        }
    }
    if (!m_api->load_game(hasGame ? &game : nullptr)) {
        std::fprintf(stderr, "vkhost: retro_load_game failed\n");
        return false;
    }
    m_contentLoaded = true;

    m_api->get_system_av_info(&m_avInfo);
    m_geometry = m_avInfo.geometry;
    if (m_pixelFormat != RETRO_PIXEL_FORMAT_XRGB8888) {
        std::fprintf(stderr, "vkhost: the core did not select XRGB8888 (the only format this host reads)\n");
        return false;
    }

    // RetroArch builds the video driver after retro_load_game: the device, then context_reset.
    if (m_hwAccepted && !StartContext()) {
        return false;
    }
    // RetroArch sets the ports' device types once the content is loaded.
    for (const auto& [port, device] : config.devices) {
        m_api->set_controller_port_device(port, device);
        std::fprintf(stderr, "vkhost: port %u device %u\n", port, device);
    }
    g_inputSeed = config.inputPattern;
    // RetroArch applies the cheats after the content (and the video driver) is up: a reset, then every cheat.
    if (!config.cheats.empty()) {
        m_api->cheat_reset();
        for (size_t i = 0; i < config.cheats.size(); ++i) {
            m_api->cheat_set(static_cast<unsigned>(i), true, config.cheats[i].c_str());
            std::fprintf(stderr, "vkhost: cheat %zu set: %s\n", i, config.cheats[i].c_str());
        }
    }
    return true;
}

bool LibretroHost::StartContext() {
    if (!m_vulkan->CreateContext(m_negotiation)) {
        std::fprintf(stderr, "vkhost: creating the Vulkan context failed\n");
        return false;
    }
    m_contextLive = true;
    ++m_contextResets;
    if (m_hwRender.context_reset != nullptr) {
        const CorePhase previous = m_vulkan->SetPhase(CorePhase::Context);
        m_hwRender.context_reset();
        m_vulkan->SetPhase(previous);
    }
    return true;
}

bool LibretroHost::ResetContext() {
    if (!m_contextLive) {
        Problem("--reset-at: there is no hardware context to reset");
        return false;
    }
    std::fprintf(stderr, "vkhost: vulkan: driver reinit (context_destroy, new device, context_reset)\n");
    ProcessDeferred(0); // the frontend finishes its frames before it tears the driver down
    if (m_hwRender.context_destroy != nullptr) {
        const CorePhase previous = m_vulkan->SetPhase(CorePhase::Context);
        m_hwRender.context_destroy();
        m_vulkan->SetPhase(previous);
    }
    m_contextLive = false;
    m_vulkan->DestroyContext();
    return StartContext();
}

void LibretroHost::Unload() {
    if (m_api) {
        // RetroArch's order: context_destroy, retro_unload_game, then the video driver frees the device
        // (destroy_device, vkDestroyDevice). On Close Content RetroArch frees the device only after it has unloaded
        // the core, so the provider refuses a core that sets destroy_device.
        if (m_contextLive) {
            ProcessDeferred(0);
            if (m_hwRender.context_destroy != nullptr) {
                const CorePhase previous = m_vulkan ? m_vulkan->SetPhase(CorePhase::Context) : CorePhase::Other;
                m_hwRender.context_destroy();
                if (m_vulkan) {
                    m_vulkan->SetPhase(previous);
                }
            }
            m_contextLive = false;
        }
        if (m_contentLoaded) {
            m_api->unload_game();
            m_contentLoaded = false;
        }
        if (m_vulkan) {
            m_vulkan->DestroyContext();
        }
        if (m_coreInitialized) {
            m_api->deinit();
            m_coreInitialized = false;
        }
    }
    if (m_vulkan) {
        m_coreSubmits = m_vulkan->CoreSubmits();
        m_lockViolations = m_vulkan->LockViolations();
        m_hiddenGpuCalls = m_vulkan->HiddenGpuCalls();
        m_hiddenSubmits = m_vulkan->HiddenSubmits();
        m_maxRunSubmits = m_vulkan->MaxRunSubmits();
        m_submitWaits = m_vulkan->SubmitWaits();
        m_failedAllocations = m_vulkan->FailedAllocations();
        m_computePipelines = m_vulkan->ComputePipelines();
        m_coreMemoryPeak = m_vulkan->CoreMemoryPeak();
        m_coreMemoryAllocations = m_vulkan->CoreMemoryAllocations();
        m_validation = m_vulkan->ValidationEnabled();
        m_problems.insert(m_problems.end(), m_vulkan->Problems().begin(), m_vulkan->Problems().end());
        // Destroying the instance can still report (leaks): read the counter afterwards.
        const auto validationErrors = m_vulkan->ValidationCounter();
        m_vulkan.reset();
        m_validationErrors = validationErrors ? validationErrors->load() : 0;
    }
    m_api.reset();
    if (m_slot >= 0 && g_hosts[m_slot] == this) {
        g_hosts[m_slot] = nullptr;
    }
    m_slot = -1;
}

bool LibretroHost::RunFrame(FrameSink* sink) {
    if (!m_api || !m_contentLoaded) {
        return false;
    }
    const bool video = (m_avEnable & RETRO_AV_ENABLE_VIDEO) != 0;
    if (m_vulkan) {
        m_vulkan->SetVideoEnabled(video);
    }
    if (m_vulkan && m_frameIndex == m_growMaskAt) {
        ProcessDeferred(0); // a mask change: the device is idle, the frontend done with every image
        m_vulkan->SetSyncSlots(3);
    }
    m_sink = sink;
    m_resetInRunPending = m_frameIndex == m_resetInRun;
    m_resetInPresentPending = m_frameIndex == m_resetInPresent;
    if (m_vulkan) {
        m_vulkan->ArmFailedSubmit(m_frameIndex);
        m_vulkan->ArmOutputImagesFailure(m_frameIndex);
        m_vulkan->ArmAllocFailure(m_frameIndex);
        m_vulkan->BeginRun();
    }
    m_api->run();
    if (m_vulkan) {
        m_vulkan->EndRun();
    }
    if (m_resetInRunPending) {
        m_resetInRunPending = false;
        Problem("--reset-in-run: the core made no environment call in the frame");
    }
    if (m_resetInPresentPending) {
        m_resetInPresentPending = false;
        Problem("--reset-in-present: the core did not query GET_AUDIO_VIDEO_ENABLE in the frame");
    }
    ++m_frameIndex;
    m_sink = nullptr;
    if (m_vulkan && m_contextLive && video) {
        m_vulkan->EndFrame();
    }
    return true;
}

bool LibretroHost::Serialize(std::vector<uint8_t>& state) {
    if (!m_api || !m_contentLoaded) {
        return false;
    }
    const CorePhase previous = m_vulkan ? m_vulkan->SetPhase(CorePhase::Serialize) : CorePhase::Other;
    const auto start = std::chrono::steady_clock::now();
    state.resize(m_api->serialize_size());
    const bool ok = !state.empty() && m_api->serialize(state.data(), state.size());
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    m_serializeSeconds += seconds;
    m_maxSerializeSeconds = std::max(m_maxSerializeSeconds, seconds);
    ++m_serializeCalls;
    if (m_vulkan) {
        m_vulkan->SetPhase(previous);
    }
    return ok;
}

bool LibretroHost::Unserialize(const std::vector<uint8_t>& state) {
    if (!m_api || !m_contentLoaded) {
        return false;
    }
    const CorePhase previous = m_vulkan ? m_vulkan->SetPhase(CorePhase::Unserialize) : CorePhase::Other;
    const auto start = std::chrono::steady_clock::now();
    const bool ok = m_api->unserialize(state.data(), state.size());
    m_unserializeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (m_vulkan) {
        m_vulkan->SetPhase(previous);
    }
    return ok;
}

uint8_t* LibretroHost::MemoryData(unsigned id, size_t& size) const {
    size = 0;
    if (!m_api || !m_contentLoaded) {
        return nullptr;
    }
    auto* data = static_cast<uint8_t*>(m_api->get_memory_data(id));
    size = data != nullptr ? m_api->get_memory_size(id) : 0;
    return data;
}

static std::string RangeProblem(const char* option, size_t offset, size_t length, size_t size) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s: 0x%zx + %zu bytes is outside RETRO_MEMORY_SYSTEM_RAM (%zu bytes)", option,
                  offset, length, size);
    return text;
}

// Where each memory map descriptor points, relative to RETRO_MEMORY_SYSTEM_RAM (once per SET_MEMORY_MAPS).
static void LogMemoryMap(const std::vector<retro_memory_descriptor>& map, const uint8_t* ram, size_t ramSize,
                         bool achievements) {
    std::fprintf(stderr,
                 "vkhost: memory: RETRO_MEMORY_SYSTEM_RAM %zu bytes, achievements %s, memory map %zu descriptors\n",
                 ramSize, achievements ? "supported" : "not declared", map.size());
    for (size_t i = 0; i < map.size(); ++i) {
        const retro_memory_descriptor& d = map[i];
        const auto* ptr = static_cast<const uint8_t*>(d.ptr) + d.offset;
        char where[64] = "outside RETRO_MEMORY_SYSTEM_RAM";
        if (d.ptr != nullptr && ram != nullptr && ptr >= ram && ptr + d.len <= ram + ramSize) {
            std::snprintf(where, sizeof(where), "RETRO_MEMORY_SYSTEM_RAM + 0x%zx", static_cast<size_t>(ptr - ram));
        }
        std::fprintf(stderr, "vkhost: memory map %zu: flags 0x%llx start 0x%08zx len 0x%zx (%s) -> %s\n", i,
                     static_cast<unsigned long long>(d.flags), static_cast<size_t>(d.start), static_cast<size_t>(d.len),
                     d.addrspace != nullptr ? d.addrspace : "", where);
    }
}

bool LibretroHost::DumpView(int frame, size_t offset, size_t length) {
    size_t size = 0;
    const uint8_t* ram = MemoryData(RETRO_MEMORY_SYSTEM_RAM, size);
    if (!m_memoryMapLogged) {
        LogMemoryMap(m_memoryMap, ram, size, m_achievements);
        m_memoryMapLogged = true;
    }
    if (ram == nullptr || offset > size || length > size - offset) {
        Problem(RangeProblem("--dump-view-at", offset, length, size));
        return false;
    }
    std::string text;
    char byte[4];
    for (size_t i = 0; i < length; ++i) {
        std::snprintf(byte, sizeof(byte), i == 0 ? "%02x" : " %02x", ram[offset + i]);
        text += byte;
    }
    std::printf("view frame %d offset 0x%06zx: %s\n", frame, offset, text.c_str());
    std::fflush(stdout);
    return true;
}

bool LibretroHost::DumpRam(int frame, uint32_t address, size_t length) {
    // The view's layout (Beetle Saturn's): Work RAM Low at 0, Work RAM High at 0x100000, host-endian 16-bit words.
    constexpr size_t kHalf = 0x100000;
    size_t offset = 0;
    if (address >= 0x00200000 && address < 0x00200000 + kHalf && length <= 0x00200000 + kHalf - address) {
        offset = address - 0x00200000;
    } else if (address >= 0x06000000 && address < 0x06000000 + kHalf && length <= 0x06000000 + kHalf - address) {
        offset = kHalf + (address - 0x06000000);
    } else {
        char text[128];
        std::snprintf(text, sizeof(text), "--dump-ram-at: 0x%08x + %zu bytes is not inside Work RAM Low or High",
                      address, length);
        Problem(text);
        return false;
    }
    size_t size = 0;
    const uint8_t* ram = MemoryData(RETRO_MEMORY_SYSTEM_RAM, size);
    if (ram == nullptr || size < 2 * kHalf) {
        Problem(RangeProblem("--dump-ram-at", offset, length, size));
        return false;
    }
    const size_t swap = std::endian::native == std::endian::little ? 1 : 0;
    std::string text;
    char byte[4];
    for (size_t i = 0; i < length; ++i) {
        std::snprintf(byte, sizeof(byte), i == 0 ? "%02x" : " %02x", ram[(offset + i) ^ swap]);
        text += byte;
    }
    std::printf("ram frame %d 0x%08x: %s\n", frame, address, text.c_str());
    std::fflush(stdout);
    return true;
}

bool LibretroHost::WriteView(int frame, size_t offset, const std::vector<uint8_t>& bytes) {
    size_t size = 0;
    uint8_t* ram = MemoryData(RETRO_MEMORY_SYSTEM_RAM, size);
    if (ram == nullptr || offset > size || bytes.size() > size - offset) {
        Problem(RangeProblem("--write-view-at", offset, bytes.size(), size));
        return false;
    }
    std::memcpy(ram + offset, bytes.data(), bytes.size());
    std::fprintf(stderr, "vkhost: wrote %zu bytes into RETRO_MEMORY_SYSTEM_RAM at 0x%06zx before frame %d\n",
                 bytes.size(), offset, frame);
    return true;
}

void LibretroHost::CoreLogLine(const char* text) {
    if (m_vulkan && std::strstr(text, "VDP1 ops-only submit after a refused frame submit") != nullptr) {
        m_vulkan->NoteOpsOnlyAfterRefused();
    }
    if (const char* line = m_vulkan ? std::strstr(text, "VDP1 sync submit fence 0x") : nullptr) {
        unsigned long long fence = 0;
        if (std::sscanf(line, "VDP1 sync submit fence 0x%llx", &fence) == 1) {
            m_vulkan->NoteSyncFence(fence);
        }
    }
    if (const char* line = std::strstr(text, "Internal resolution: frames at ")) {
        unsigned scale = 0;
        if (std::sscanf(line, "Internal resolution: frames at %ux", &scale) == 1 && scale >= 1 && scale <= 4) {
            m_frameScale = scale;
        }
    }
}

void LibretroHost::SetOption(const std::string& key, const std::string& value) {
    m_overrides[key] = value;
    m_optionsUpdated = true;
    std::fprintf(stderr, "vkhost: option %s=%s (live change)\n", key.c_str(), value.c_str());
}

bool LibretroHost::ProbeLine(const char* text) {
    const char* line = std::strstr(text, "VDP2 probe: ");
    ProbeFrame f;
    const int n = std::sscanf(line,
                              "VDP2 probe: batches %u arena %u copy %u mosaic %u vcell %u chardelay %u ddlinescroll %u "
                              "extcc %u shadow %u mesh %u crammodes %u front_us %lf vdp1_front_us %lf "
                              "vdp1_record_us %lf vdp1_gpu_us %lf",
                              &f.batches, &f.arenaBytes, &f.copyBytes, &f.mosaicLines, &f.vcellLines,
                              &f.charPatDelayLines, &f.ddLineScrollLines, &f.extendedCCLines, &f.shadowLines,
                              &f.meshSlices, &f.cramModeMask, &f.frontMicroseconds, &f.vdp1FrontMicroseconds,
                              &f.vdp1RecordMicroseconds, &f.vdp1GpuMicroseconds);
    if (n != 12 && n != 15) { // 12: a core without the VDP1 times
        return false;
    }
    if (m_sink != nullptr) {
        m_probeFrames.push_back(f);
    }
    return true;
}

bool LibretroHost::Vdp1ProbeLine(const char* text) {
    const char* line = std::strstr(text, "VDP1 probe: ");
    unsigned long long v[19] = {};
    const int n = std::sscanf(line,
                              "VDP1 probe: commands %llu lines %llu pixels %llu syncs %llu dirtysyncs %llu fbwrites %llu "
                              "erases %llu batches opkind %llu hazard %llu swap %llu capacity %llu boundary %llu take %llu "
                              "nx rect %llu quad %llu replicate %llu slots %llu subbatches %llu gen_ns %llu",
                              &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12],
                              &v[13], &v[14], &v[15], &v[16], &v[17], &v[18]);
    if (n != 7 && n != 13 && n != 19) { // 7: a core without the batch counters; 13: without the N x counters
        return false;
    }
    if (m_sink != nullptr) {
        Vdp1ProbeFrame f{v[0], v[1], v[2], v[3], v[4], v[5], v[6], {}};
        for (int i = 0; i < 6; ++i) {
            f.batchEnds[i] = v[7 + i];
        }
        for (int i = 0; i < 3; ++i) {
            f.nxSpans[i] = v[13 + i];
        }
        f.nxSlots = v[16];
        f.nxSubBatches = v[17];
        f.nxGenerateNanos = v[18];
        m_vdp1ProbeFrames.push_back(f);
    }
    return true;
}

bool LibretroHost::PassLine(const char* text) {
    // "GPU passes: name value name value ... overflow N"
    std::istringstream in(std::strstr(text, "GPU passes: ") + std::strlen("GPU passes: "));
    std::vector<std::string> names;
    PassFrame f;
    std::string name;
    double value = 0;
    while (in >> name >> value) {
        if (name == "overflow") {
            f.overflow = static_cast<uint64_t>(value);
            break;
        }
        names.push_back(name);
        f.micros.push_back(value);
    }
    if (names.empty()) {
        return false;
    }
    if (m_sink != nullptr) {
        if (m_passNames.empty()) {
            m_passNames = names;
        }
        if (names == m_passNames) {
            m_passFrames.push_back(std::move(f));
        }
    }
    return true;
}

void LibretroHost::Problem(const std::string& text) {
    // Each kind once; the counters carry the totals.
    if (std::find(m_problems.begin(), m_problems.end(), text) == m_problems.end()) {
        m_problems.push_back(text);
        std::fprintf(stderr, "vkhost: problem: %s\n", text.c_str());
    }
}

void LibretroHost::FillReport(RunReport& report) const {
    report.recordedFrames += m_recordedFrames;
    report.recordedHardwareFrames += m_recordedHardware;
    report.contextResets += m_contextResets;
    report.unreadableFrames += m_unreadable;
    report.readbackSeconds += m_readbackSeconds;
    report.systemAvInfoCalls += m_systemAvInfoCalls;
    report.geometryCalls += m_geometryCalls;
    report.hiddenGpuCalls += m_hiddenFrames;
    report.serializeCalls += m_serializeCalls;
    report.serializeSeconds += m_serializeSeconds;
    report.maxSerializeSeconds = std::max(report.maxSerializeSeconds, m_maxSerializeSeconds);
    report.unserializeSeconds += m_unserializeSeconds;
    report.probeFrames.insert(report.probeFrames.end(), m_probeFrames.begin(), m_probeFrames.end());
    report.vdp1ProbeFrames.insert(report.vdp1ProbeFrames.end(), m_vdp1ProbeFrames.begin(), m_vdp1ProbeFrames.end());
    if (report.passNames.empty() || report.passNames == m_passNames) {
        if (report.passNames.empty()) {
            report.passNames = m_passNames;
        }
        report.passFrames.insert(report.passFrames.end(), m_passFrames.begin(), m_passFrames.end());
    }
    report.problems.insert(report.problems.end(), m_problems.begin(), m_problems.end());
    if (m_vulkan) {
        report.coreSubmits += m_vulkan->CoreSubmits();
        report.lockViolations += m_vulkan->LockViolations();
        report.hiddenGpuCalls += m_vulkan->HiddenGpuCalls();
        report.hiddenSubmits += m_vulkan->HiddenSubmits();
        report.maxRunSubmits = std::max(report.maxRunSubmits, m_vulkan->MaxRunSubmits());
        report.submitWaits.Add(m_vulkan->SubmitWaits());
        report.validation = report.validation || m_vulkan->ValidationEnabled();
        report.validationErrors += m_vulkan->ValidationErrors();
        report.problems.insert(report.problems.end(), m_vulkan->Problems().begin(), m_vulkan->Problems().end());
    } else {
        report.coreSubmits += m_coreSubmits;
        report.lockViolations += m_lockViolations;
        report.hiddenGpuCalls += m_hiddenGpuCalls;
        report.hiddenSubmits += m_hiddenSubmits;
        report.maxRunSubmits = std::max(report.maxRunSubmits, m_maxRunSubmits);
        report.submitWaits.Add(m_submitWaits);
        report.validation = report.validation || m_validation;
        report.validationErrors += m_validationErrors;
    }
}

template <int Slot>
bool RETRO_CALLCONV LibretroHost::EnvironmentThunk(unsigned cmd, void* data) {
    return g_hosts[Slot] != nullptr && g_hosts[Slot]->Environment(cmd, data);
}

template <int Slot>
void RETRO_CALLCONV LibretroHost::VideoThunk(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (g_hosts[Slot] != nullptr) {
        g_hosts[Slot]->Video(data, width, height, pitch);
    }
}

void RETRO_CALLCONV LibretroHost::AudioSampleThunk(int16_t, int16_t) {}

size_t RETRO_CALLCONV LibretroHost::AudioBatchThunk(const int16_t*, size_t frames) {
    return frames;
}

void RETRO_CALLCONV LibretroHost::InputPollThunk() {}

int16_t RETRO_CALLCONV LibretroHost::InputStateThunk(unsigned port, unsigned device, unsigned index, unsigned id) {
    return g_inputSeed != 0 ? ScriptedInput(port, device, index, id) : 0;
}

void LibretroHost::SetInputFrame(int frame) {
    g_inputFrame = frame;
}

void LibretroHost::LogUnsupportedOnce(unsigned cmd) {
    if (m_unsupportedLogged.insert(cmd).second) {
        std::fprintf(stderr, "vkhost: environment call %u%s not supported (returning false)\n", cmd & 0xFFFFu,
                     (cmd & RETRO_ENVIRONMENT_EXPERIMENTAL) != 0 ? " (experimental)" : "");
    }
}

void LibretroHost::StoreOptionDefaults(const retro_core_options_v2& options) {
    m_defaults.clear();
    for (const retro_core_option_v2_definition* def = options.definitions; def != nullptr && def->key != nullptr;
         ++def) {
        const char* value = def->default_value != nullptr ? def->default_value : def->values[0].value;
        if (value != nullptr) {
            m_defaults[def->key] = value;
        }
    }
}

bool LibretroHost::Environment(unsigned rawCmd, void* data) {
    const unsigned cmd = rawCmd & ~(RETRO_ENVIRONMENT_EXPERIMENTAL | RETRO_ENVIRONMENT_PRIVATE);
    switch (cmd) {
    case kEnv(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION):
        *static_cast<unsigned*>(data) = 2;
        return true;
    case kEnv(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2):
        if (data == nullptr) {
            return false;
        }
        StoreOptionDefaults(*static_cast<const retro_core_options_v2*>(data));
        return true;
    case kEnv(RETRO_ENVIRONMENT_GET_VARIABLE): {
        auto* var = static_cast<retro_variable*>(data);
        var->value = nullptr;
        if (var->key == nullptr) {
            return false;
        }
        if (const auto it = m_overrides.find(var->key); it != m_overrides.end()) {
            var->value = it->second.c_str();
            return true;
        }
        if (const auto it = m_defaults.find(var->key); it != m_defaults.end()) {
            var->value = it->second.c_str();
            return true;
        }
        return false;
    }
    case kEnv(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE):
        *static_cast<bool*>(data) = m_optionsUpdated;
        m_optionsUpdated = false;
        if (m_resetInRunPending) {
            // A driver reinit from inside retro_run, the way RetroArch runs one inside SET_SYSTEM_AV_INFO: the
            // core gets context_destroy and context_reset while its own call is still in progress.
            m_resetInRunPending = false;
            ResetContext();
        }
        return true;
    case kEnv(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME):
    case kEnv(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS):
    case kEnv(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE):
        return true;
    case kEnv(RETRO_ENVIRONMENT_SET_MEMORY_MAPS): {
        const auto* map = static_cast<const retro_memory_map*>(data);
        m_memoryMap.assign(map->descriptors, map->descriptors + map->num_descriptors);
        m_memoryMapLogged = false;
        return true;
    }
    case kEnv(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS):
        m_achievements = *static_cast<const bool*>(data);
        return true;
    case kEnv(RETRO_ENVIRONMENT_GET_LOG_INTERFACE):
        static_cast<retro_log_callback*>(data)->log = m_slot == 0 ? &LogCallback<0> : &LogCallback<1>;
        return true;
    case kEnv(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER):
        if (m_vulkan) {
            // --preferred-hw gl: RetroArch on the gl driver with driver switching allowed (it would still accept
            // SET_HW_RENDER and switch to vulkan).
            *static_cast<unsigned*>(data) = m_preferGl ? RETRO_HW_CONTEXT_OPENGL : RETRO_HW_CONTEXT_VULKAN;
            return true;
        }
        if (m_refuseHw) {
            // RetroArch on the gl driver with driver switching disabled: reports gl, returns false.
            *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGL;
        }
        return false;
    case kEnv(RETRO_ENVIRONMENT_SET_HW_RENDER): {
        auto* callback = static_cast<retro_hw_render_callback*>(data);
        if (!m_vulkan || m_refuseHw || callback->context_type != RETRO_HW_CONTEXT_VULKAN) {
            std::fprintf(stderr, "vkhost: SET_HW_RENDER (context type %d) refused\n",
                         static_cast<int>(callback->context_type));
            return false;
        }
        if (m_contentLoaded) {
            Problem("SET_HW_RENDER outside retro_load_game");
        }
        // Not used with Vulkan; RetroArch fills them for GL.
        callback->get_current_framebuffer = nullptr;
        callback->get_proc_address = nullptr;
        m_hwRender = *callback;
        m_hwAccepted = true;
        std::fprintf(stderr, "vkhost: SET_HW_RENDER Vulkan %u.%u accepted\n", callback->version_major,
                     callback->version_minor);
        return true;
    }
    case kEnv(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT): {
        auto* support = static_cast<retro_hw_render_context_negotiation_interface*>(data);
        if (!m_vulkan || support->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN) {
            return false;
        }
        support->interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
        return true;
    }
    case kEnv(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE):
        // RetroArch refuses it in a run-ahead second instance, like SET_HW_RENDER. Only the pointer is kept: the
        // core's struct must be static (it is used again at every driver reinit).
        if (!m_vulkan || m_refuseHw) {
            return false;
        }
        m_negotiation = static_cast<const retro_hw_render_context_negotiation_interface*>(data);
        return true;
    case kEnv(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE):
        if (!m_vulkan || !m_contextLive) {
            return false;
        }
        *static_cast<const retro_hw_render_interface**>(data) = m_vulkan->Interface();
        return true;
    case kEnv(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS):
        return true;
    case kEnv(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION):
        *static_cast<unsigned*>(data) = 1;
        return true;
    case kEnv(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT): {
        const auto format = *static_cast<const retro_pixel_format*>(data);
        if (format != RETRO_PIXEL_FORMAT_XRGB8888) {
            std::fprintf(stderr, "vkhost: pixel format %d refused (only XRGB8888 is supported)\n",
                         static_cast<int>(format));
            return false;
        }
        m_pixelFormat = format;
        return true;
    }
    case kEnv(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY):
        *static_cast<const char**>(data) = m_systemDir.c_str();
        return true;
    case kEnv(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY):
        *static_cast<const char**>(data) = m_saveDir.c_str();
        return true;
    case kEnv(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO): {
        const auto& info = *static_cast<const retro_system_av_info*>(data);
        // RetroArch reinits the video driver (context_destroy, context_reset) inside the call when the maximum
        // geometry or the frame rate changes.
        const bool reinit = m_contextLive && (info.geometry.max_width != m_avInfo.geometry.max_width ||
                                              info.geometry.max_height != m_avInfo.geometry.max_height ||
                                              info.timing.fps != m_avInfo.timing.fps);
        m_avInfo = info;
        m_geometry = m_avInfo.geometry;
        ++m_systemAvInfoCalls;
        std::fprintf(stderr, "vkhost: geometry: SET_SYSTEM_AV_INFO %ux%u (max %ux%u) %.2f fps\n",
                     m_geometry.base_width, m_geometry.base_height, m_geometry.max_width, m_geometry.max_height,
                     m_avInfo.timing.fps);
        if (reinit) {
            ResetContext();
        }
        return true;
    }
    case kEnv(RETRO_ENVIRONMENT_SET_GEOMETRY):
        m_geometry = *static_cast<const retro_game_geometry*>(data);
        m_avInfo.geometry = m_geometry;
        ++m_geometryCalls;
        std::fprintf(stderr, "vkhost: geometry: SET_GEOMETRY %ux%u (max %ux%u, aspect %.4f)\n", m_geometry.base_width,
                     m_geometry.base_height, m_geometry.max_width, m_geometry.max_height,
                     static_cast<double>(m_geometry.aspect_ratio));
        return true;
    case kEnv(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE):
        if (m_resetInPresentPending) {
            // The reinit after the core emulated its frame (its renderer ended the VDP2 frame) and before it
            // recorded and presented it.
            m_resetInPresentPending = false;
            ResetContext();
        }
        *static_cast<int*>(data) = m_avEnable;
        return true;
    default:
        LogUnsupportedOnce(rawCmd);
        return false;
    }
}

void LibretroHost::ProcessDeferred(size_t keep) {
    while (m_deferred.size() > keep) {
        DeferredFrame entry = m_deferred.front();
        m_deferred.erase(m_deferred.begin());
        HostFrame frame;
        const auto readStart = std::chrono::steady_clock::now();
        const bool readOk = m_vulkan && m_vulkan->ReadDeferred(entry.image, frame);
        m_readbackSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - readStart).count();
        if (!readOk) {
            ++m_unreadable;
            Problem("--defer-image-read: a set_image image could not be read when the frontend got to it");
            continue;
        }
        frame.scale = entry.scale;
        m_lastFrame = frame;
        if (entry.sink != nullptr) {
            ++m_recordedFrames;
            m_recordedHardware += entry.dupe ? 0 : 1; // a dupe is displayed again, not a new hardware frame
            entry.sink->OnFrame(std::move(frame));
        }
    }
}

void LibretroHost::Video(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (data == RETRO_HW_FRAME_BUFFER_VALID && m_vulkan && m_contextLive) {
        m_vulkan->CheckFrameExtent(width, height);
    }
    if (data == nullptr && m_deferImageRead > 0 && m_contextLive) {
        ++m_duplicate;
        // A frontend repeats the image it holds (the last set_image) and reads it again on its own schedule.
        if (!m_haveCaptured) {
            return;
        }
        DeferredFrame entry;
        entry.image = m_lastCaptured;
        entry.sink = m_sink;
        entry.scale = m_lastCapturedScale;
        entry.dupe = true;
        m_vulkan->PinImage(entry.image);
        m_deferred.push_back(entry);
        ProcessDeferred(static_cast<size_t>(m_deferImageRead));
        return;
    }
    if (data == RETRO_HW_FRAME_BUFFER_VALID && m_deferImageRead > 0 && m_vulkan && m_contextLive) {
        if ((m_avEnable & RETRO_AV_ENABLE_VIDEO) == 0) {
            ++m_hiddenFrames;
            Problem("hardware frame in a frame with video disabled");
        }
        ++m_displayed;
        ++m_hardwareFrames;
        // Kept as a threaded frontend keeps it (the handles copied) and read N displayed frames later.
        DeferredFrame entry;
        entry.sink = m_sink;
        entry.scale = m_frameScale;
        if (!m_vulkan->CaptureImage(width, height, entry.image)) {
            ++m_unreadable;
            Problem("hardware frame without a readable set_image image");
            return;
        }
        m_lastCaptured = entry.image;
        m_lastCapturedScale = entry.scale;
        m_haveCaptured = true;
        m_deferred.push_back(entry);
        ProcessDeferred(static_cast<size_t>(m_deferImageRead));
        return;
    }
    if (data != nullptr) {
        m_haveCaptured = false;
        ProcessDeferred(0); // a software frame: everything handed over before it is shown first
    }
    if (data == nullptr) {
        ++m_duplicate; // the core asks the frontend to repeat the last frame
        if (m_sink != nullptr && m_lastFrame.w != 0) {
            // Displayed again, as a frontend does.
            ++m_recordedFrames;
            HostFrame frame = m_lastFrame;
            m_sink->OnFrame(std::move(frame));
        }
        return;
    }
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        if (!m_vulkan || !m_contextLive) {
            ++m_unreadable;
            Problem("hardware frame without a hardware context");
            return;
        }
        if ((m_avEnable & RETRO_AV_ENABLE_VIDEO) == 0) {
            ++m_hiddenFrames;
            Problem("hardware frame in a frame with video disabled");
        }
        ++m_displayed;
        ++m_hardwareFrames;
        if (m_sink == nullptr) {
            return;
        }
        // The image the core handed over with set_image, read in GENERAL layout after its work completed.
        HostFrame frame;
        const auto readStart = std::chrono::steady_clock::now();
        const bool readOk = m_vulkan->ReadImage(width, height, frame);
        m_readbackSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - readStart).count();
        if (!readOk) {
            ++m_unreadable;
            Problem("hardware frame without a readable set_image image");
            return;
        }
        ++m_recordedFrames;
        ++m_recordedHardware;
        frame.scale = m_frameScale;
        m_lastFrame = frame;
        m_sink->OnFrame(std::move(frame));
        return;
    }
    ++m_displayed;
    if (m_sink == nullptr) {
        return;
    }
    ++m_recordedFrames;
    HostFrame frame;
    frame.w = width;
    frame.h = height;
    frame.xrgb.resize(static_cast<size_t>(width) * height);
    const auto* rows = static_cast<const uint8_t*>(data);
    for (unsigned y = 0; y < height; ++y) {
        std::memcpy(&frame.xrgb[static_cast<size_t>(y) * width], rows + static_cast<size_t>(y) * pitch,
                    static_cast<size_t>(width) * sizeof(uint32_t));
    }
    frame.scale = m_frameScale;
    m_lastFrame = frame;
    m_sink->OnFrame(std::move(frame));
}

} // namespace vkhost
