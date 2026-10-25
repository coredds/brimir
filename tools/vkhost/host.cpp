// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "host.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
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

LibretroHost* g_host = nullptr;
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

void RETRO_CALLCONV LogCallback(enum retro_log_level level, const char* fmt, ...) {
    if (level == RETRO_LOG_DEBUG && !g_verbose) {
        return;
    }
    static const char* const names[] = {"debug", "info", "warn", "error"};
    const unsigned index = static_cast<unsigned>(level);
    std::fprintf(stderr, "[core %s] ", index < 4 ? names[index] : "?");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
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
};

LibretroHost::LibretroHost() = default;

LibretroHost::~LibretroHost() {
    Unload();
}

bool LibretroHost::Load(const HostConfig& config, const std::string& systemDir, const std::string& saveDir) {
    if (g_host != nullptr) {
        std::fprintf(stderr, "vkhost: only one core can be loaded at a time\n");
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
    if (missing) {
        return false;
    }
    if (api->api_version() != RETRO_API_VERSION) {
        std::fprintf(stderr, "vkhost: core reports libretro API version %u, expected %u\n", api->api_version(),
                     static_cast<unsigned>(RETRO_API_VERSION));
        return false;
    }

    m_api = std::move(api);
    g_host = this;

    // RetroArch order: environment, init, the other callbacks, then the content.
    m_api->set_environment(&LibretroHost::EnvironmentThunk);
    m_api->init();
    m_coreInitialized = true;
    m_api->set_video_refresh(&LibretroHost::VideoThunk);
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
    return true;
}

void LibretroHost::Unload() {
    if (m_api) {
        if (m_contentLoaded) {
            m_api->unload_game();
            m_contentLoaded = false;
        }
        if (m_coreInitialized) {
            m_api->deinit();
            m_coreInitialized = false;
        }
        m_api.reset();
    }
    if (g_host == this) {
        g_host = nullptr;
    }
}

bool LibretroHost::RunFrame(FrameSink* sink) {
    if (!m_api || !m_contentLoaded) {
        return false;
    }
    m_sink = sink;
    m_api->run();
    m_sink = nullptr;
    return true;
}

bool RETRO_CALLCONV LibretroHost::EnvironmentThunk(unsigned cmd, void* data) {
    return g_host != nullptr && g_host->Environment(cmd, data);
}

void RETRO_CALLCONV LibretroHost::VideoThunk(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (g_host != nullptr) {
        g_host->Video(data, width, height, pitch);
    }
}

void RETRO_CALLCONV LibretroHost::AudioSampleThunk(int16_t, int16_t) {}

size_t RETRO_CALLCONV LibretroHost::AudioBatchThunk(const int16_t*, size_t frames) {
    return frames;
}

void RETRO_CALLCONV LibretroHost::InputPollThunk() {}

int16_t RETRO_CALLCONV LibretroHost::InputStateThunk(unsigned, unsigned, unsigned, unsigned) {
    return 0;
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
        *static_cast<bool*>(data) = false;
        return true;
    case kEnv(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME):
    case kEnv(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS):
    case kEnv(RETRO_ENVIRONMENT_SET_MEMORY_MAPS):
    case kEnv(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE):
        return true;
    case kEnv(RETRO_ENVIRONMENT_GET_LOG_INTERFACE):
        static_cast<retro_log_callback*>(data)->log = &LogCallback;
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
    case kEnv(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO):
        m_avInfo = *static_cast<const retro_system_av_info*>(data);
        m_geometry = m_avInfo.geometry;
        if (m_verbose) {
            std::fprintf(stderr, "vkhost: SET_SYSTEM_AV_INFO %ux%u (max %ux%u) %.2f fps\n", m_geometry.base_width,
                         m_geometry.base_height, m_geometry.max_width, m_geometry.max_height, m_avInfo.timing.fps);
        }
        return true;
    case kEnv(RETRO_ENVIRONMENT_SET_GEOMETRY):
        m_geometry = *static_cast<const retro_game_geometry*>(data);
        m_avInfo.geometry = m_geometry;
        if (m_verbose) {
            std::fprintf(stderr, "vkhost: SET_GEOMETRY %ux%u (max %ux%u)\n", m_geometry.base_width,
                         m_geometry.base_height, m_geometry.max_width, m_geometry.max_height);
        }
        return true;
    case kEnv(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE):
        *static_cast<int*>(data) = m_avEnable;
        return true;
    default:
        LogUnsupportedOnce(rawCmd);
        return false;
    }
}

void LibretroHost::Video(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (data == nullptr) {
        ++m_duplicate; // the core asks the frontend to repeat the last frame
        return;
    }
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        // Task 6: read back the retro_vulkan_image the core handed over with set_image.
        ++m_unreadable;
        if (m_unreadable == 1) {
            std::fprintf(stderr, "vkhost: hardware frame received but no hardware context is implemented\n");
        }
        return;
    }
    ++m_displayed;
    if (m_sink == nullptr) {
        return;
    }
    HostFrame frame;
    frame.w = width;
    frame.h = height;
    frame.xrgb.resize(static_cast<size_t>(width) * height);
    const auto* rows = static_cast<const uint8_t*>(data);
    for (unsigned y = 0; y < height; ++y) {
        std::memcpy(&frame.xrgb[static_cast<size_t>(y) * width], rows + static_cast<size_t>(y) * pitch,
                    static_cast<size_t>(width) * sizeof(uint32_t));
    }
    m_sink->OnFrame(std::move(frame));
}

} // namespace vkhost
