// Brimir - Sega Saturn libretro core based on Ymir
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "libretro.h"
#include "options.hpp"
#include "brimir/cheat_engine.hpp"
#include "brimir/core_wrapper.hpp"
#include "brimir/lockstep.hpp"
#include "brimir/ram_view.hpp"

#ifdef BRIMIR_VULKAN
#include "libretro_vulkan.hpp"
#include <gpu/vdp/vulkan_vdp_renderer.hpp>
#endif

#if BRIMIR_VDP1_PROBE
#include <ymir/hw/vdp/renderer/vdp1_probe.hpp>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Libretro callbacks
static retro_log_printf_t log_cb = nullptr;
static retro_video_refresh_t video_cb = nullptr;
static retro_audio_sample_t audio_cb = nullptr;
static retro_audio_sample_batch_t audio_batch_cb = nullptr;
static retro_input_poll_t input_poll_cb = nullptr;
static retro_input_state_t input_state_cb = nullptr;
static retro_environment_t environ_cb = nullptr;

// Core instance
static std::unique_ptr<brimir::CoreWrapper> g_core;

// Guards any libretro API call that inspects or mutates core state.
// This protects against frontends that dispatch save/load/reset while the
// core thread is still running a frame (RetroArch normally serializes these,
// but the lock makes the core safe either way).
static std::recursive_mutex g_coreMutex;

// Beetle-compatible work RAM view (brimir/ram_view.hpp): RETRO_MEMORY_SYSTEM_RAM and the memory map point into it, so
// achievement sets and RetroArch cheats authored on Beetle Saturn read and write the same bytes. Its address never
// changes (the frontend keeps the pointers). Order in retro_run: apply_ram_view_writes, apply_cheats, RunFrame,
// publish_ram_view.
static brimir::RamView g_ramView;

// Action Replay codes from retro_cheat_set (brimir/cheat_engine.hpp). Frontend state: not in save states, cleared by
// retro_cheat_reset and when the content is unloaded.
static brimir::CheatEngine g_cheats;

// Memory descriptors for achievements (rcheevos maps Saturn $000000/$100000 to 0x00200000/0x06000000) and RetroArch's
// cheats and memory viewer (which concatenate the RETRO_MEMDESC_SYSTEM_RAM descriptors in order: Low then High, the
// same offsets as RETRO_MEMORY_SYSTEM_RAM). Field order as in libretro.h (flags, ptr, offset, start, select,
// disconnect, len, addrspace). No save RAM descriptor: RetroArch reads and writes the .srm through
// retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), and the backup RAM is not at a work RAM address.
static struct retro_memory_descriptor g_memdesc[] = {
    { RETRO_MEMDESC_SYSTEM_RAM, nullptr, 0, 0x00200000, 0, 0, brimir::RamView::kHalfSize, "WRAML" },
    { RETRO_MEMDESC_SYSTEM_RAM, nullptr, 0, 0x06000000, 0, 0, brimir::RamView::kHalfSize, "WRAMH" },
};
static const unsigned g_memdesc_count = sizeof(g_memdesc) / sizeof(g_memdesc[0]);

// Whether the frontend supports RETRO_DEVICE_ID_JOYPAD_MASK (single-call input read)
static bool g_input_bitmask_supported = false;

// Controller ports (brimir/peripheral_input.hpp): the device type of each port (retro_set_controller_port_device), kept
// across content and applied to the core when it exists.
static brimir::PeripheralKind g_portKinds[2] = {brimir::PeripheralKind::ControlPad, brimir::PeripheralKind::ControlPad};

// Per-content frame state. Reset on load/unload so a new game always triggers
// RETRO_ENVIRONMENT_SET_GEOMETRY on its first frame, even at the same resolution.
static unsigned int g_lastWidth = 0;
static unsigned int g_lastHeight = 0;
static size_t g_profileFrameCount = 0;

static void reset_frame_state(void);

// Largest frame the core reports (SET_GEOMETRY ignores max_*; changing them through SET_SYSTEM_AV_INFO reinits the
// frontend's video driver, so they are fixed per content).
static constexpr unsigned kMaxWidth = 704;
static constexpr unsigned kMaxHeight = 512;

#ifdef BRIMIR_VULKAN
// Vulkan frame output (brimir_renderer = vulkan, src/libretro/libretro_vulkan.cpp). g_vulkanRequested: the frontend
// accepted SET_HW_RENDER for the loaded content. The output is live between context_reset and context_destroy;
// the frontend calls both after retro_load_game, possibly from inside other libretro calls (driver reinit).
static bool g_vulkanRequested = false;
static std::unique_ptr<brimir::libretro::VulkanOutput> g_vulkanOutput;
static bool g_vulkanPresentErrorLogged = false;
// The Vulkan VDP renderer (VDP2 on the GPU) could not be installed or attached on the current context: software
// renderer until the next context_reset.
static bool g_vulkanRendererFailed = false;
// Declared at content load whenever Vulkan is requested, whatever brimir_internal_resolution is: 704 x 4 (the largest
// frame at 4x), square so a 90/270 rotation fits. A later change of the option is then a base-size change only
// (SET_GEOMETRY); a larger maximum would need SET_SYSTEM_AV_INFO, which reinits the frontend's video driver.
static constexpr unsigned kVulkanMaxSize = 704 * 4;
// Internal resolution of the last frame handed over (0: none yet), for the log line vkhost --hash-sample follows.
static unsigned g_presentedScale = 0;
// brimir_internal_resolution above 1 without the Vulkan VDP2 renderer has no effect: said once per content.
static bool g_internalResolutionNoopLogged = false;
// retro_load_game is applying the options before it knows whether Vulkan is requested.
static bool g_loadingContent = false;
// GPU work outside the frame submits (VDP1 on the GPU's sync submits) is allowed inside retro_run and the context
// callbacks only (VulkanOutput::SetGpuWorkAllowed); scopes nest (a driver reinit inside retro_run).
static bool g_gpuWorkAllowed = false;
class GpuWorkScope {
public:
    GpuWorkScope() : m_previous(g_gpuWorkAllowed) {
        g_gpuWorkAllowed = true;
        Apply();
    }
    ~GpuWorkScope() {
        g_gpuWorkAllowed = m_previous;
        Apply();
    }
    GpuWorkScope(const GpuWorkScope&) = delete;
    GpuWorkScope& operator=(const GpuWorkScope&) = delete;
    static void Apply() {
        if (g_vulkanOutput) {
            g_vulkanOutput->SetGpuWorkAllowed(g_gpuWorkAllowed);
        }
    }

private:
    bool m_previous;
};
#endif

// Work RAM Low and High (Saturn byte order, 1 MiB each), or false if the core has no Saturn.
static bool work_ram(uint8_t*& low, uint8_t*& high) {
    if (!g_core) {
        return false;
    }
    low = static_cast<uint8_t*>(g_core->GetSystemRAMRawPointer());
    high = static_cast<uint8_t*>(g_core->GetSystemRAMHighRawPointer());
    return low != nullptr && high != nullptr && g_core->GetSystemRAMSize() == brimir::RamView::kHalfSize &&
           g_core->GetSystemRAMHighSize() == brimir::RamView::kHalfSize;
}

// After a frame, a state load, a reset and a content load: the view shows the current work RAM, and bytes the
// frontend changed in it before are dropped.
static void publish_ram_view(void) {
    uint8_t* low = nullptr;
    uint8_t* high = nullptr;
    if (!work_ram(low, high)) {
        g_ramView.Clear();
        return;
    }
    g_ramView.Publish(std::span<const uint8_t, brimir::RamView::kHalfSize>(low, brimir::RamView::kHalfSize),
                      std::span<const uint8_t, brimir::RamView::kHalfSize>(high, brimir::RamView::kHalfSize));
}

// Before a frame: the bytes the frontend wrote into the view since the last publish (RetroArch cheats, memory editor,
// achievement tools) go into work RAM.
static void apply_ram_view_writes(void) {
    uint8_t* low = nullptr;
    uint8_t* high = nullptr;
    if (!work_ram(low, high)) {
        return;
    }
    g_ramView.ApplyFrontendWrites(std::span<uint8_t, brimir::RamView::kHalfSize>(low, brimir::RamView::kHalfSize),
                                  std::span<uint8_t, brimir::RamView::kHalfSize>(high, brimir::RamView::kHalfSize));
}

// Before a frame, after the frontend's writes: the enabled Action Replay cheats write work RAM.
static void apply_cheats(void) {
    uint8_t* low = nullptr;
    uint8_t* high = nullptr;
    if (g_cheats.ActiveCount() == 0 || !work_ram(low, high)) {
        return;
    }
    g_cheats.Apply(std::span<uint8_t, brimir::RamView::kHalfSize>(low, brimir::RamView::kHalfSize),
                   std::span<uint8_t, brimir::RamView::kHalfSize>(high, brimir::RamView::kHalfSize));
}

static void reset_frame_state(void) {
    g_lastWidth = 0;
    g_lastHeight = 0;
    g_profileFrameCount = 0;
#ifdef BRIMIR_VULKAN
    g_presentedScale = 0;
    g_internalResolutionNoopLogged = false;
#endif
}

static unsigned max_geometry_width(void) {
#ifdef BRIMIR_VULKAN
    if (g_vulkanRequested) return kVulkanMaxSize;
#endif
    return kMaxWidth;
}

static unsigned max_geometry_height(void) {
#ifdef BRIMIR_VULKAN
    if (g_vulkanRequested) return kVulkanMaxSize;
#endif
    return kMaxHeight;
}

// Helper function for logging
static void brimir_log(retro_log_level level, const char* fmt, ...) {
    if (!log_cb) return;
    
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    log_cb(level, "[Brimir] %s\n", buffer);
}

// Disk control callback forward declarations
static bool disk_set_eject_state(bool ejected);
static bool disk_get_eject_state(void);
static unsigned disk_get_image_index(void);
static bool disk_set_image_index(unsigned index);
static unsigned disk_get_num_images(void);
static bool disk_replace_image_index(unsigned index, const struct retro_game_info* info);
static bool disk_add_image_index(void);
static bool disk_set_initial_image(unsigned index, const char* path);
static bool disk_get_image_path(unsigned index, char* s, size_t len);
static bool disk_get_image_label(unsigned index, char* s, size_t len);


// Helper function to get option value
static const char* get_option_value(const char* key, const char* default_value = nullptr) {
    if (!environ_cb) return default_value;
    
    struct retro_variable var = { key, nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        return var.value;
    }
    return default_value;
}

// Cached core option values so Quick Menu changes can be applied live each frame.
// Defaults here must stay in sync with src/libretro/options.cpp for each key.
struct OptionCache {
    std::string audio_interp = "linear";
    std::string cd_speed = "2";
    std::string sh2_overclock = "100";
    std::string autodetect_region = "enabled";
    BrimirDeinterlaceOptions deinterlace;
    std::string audio_volume = "100";
    std::string rotation = "0";
    std::string overscan = "0";
    std::string profiling = "disabled";
    std::string cd_preload = "enabled";
    std::string threaded_vdp1 = "enabled";
    std::string threaded_vdp2 = "enabled";
    std::string sh2_jit = "disabled";
    std::string sh2_jit_compile_thread = "auto";
    // Load-time only (read in retro_load_game); kept to report live changes.
    std::string renderer = "software";
    std::string rtc_mode = "virtual";
    std::string cartridge = "auto";
    std::string sh2_cache = "disabled";
    std::string cdblock_lle = "disabled";
    // Live: SCSP step granularity.
    std::string scsp_granularity = "0";
    // Live (Vulkan VDP2 renderer only): applied at the next shown frame.
    std::string internal_resolution = "1";
    // Live (Vulkan renderer with VDP1 on the GPU, internal resolution above 1): applied at the next shown frame.
    std::string vdp1_upscaling = "enabled";
    // Live: the next shown frame.
    std::string gun_crosshair = "cross";
    // Live: the next frame's input (CoreWrapper::SetInputSettings).
    std::string analog_deadzone = "15";
    std::string trigger_deadzone = "15";
    std::string mouse_sensitivity = "100";
} g_options;

// Light gun crosshairs of this frame (option brimir_gun_crosshair), drawn on the shown frame only.
static brimir::Crosshair g_crosshairs[2];
static unsigned g_crosshairCount = 0;

static brimir::CrosshairStyle crosshair_style(void) {
    if (g_options.gun_crosshair == "off") {
        return brimir::CrosshairStyle::Off;
    }
    return g_options.gun_crosshair == "dot" ? brimir::CrosshairStyle::Dot : brimir::CrosshairStyle::Cross;
}

static void update_crosshairs(const brimir::InputSnapshot (&inputs)[2]) {
    g_crosshairCount = 0;
    const brimir::CrosshairStyle style = crosshair_style();
    for (unsigned port = 0; port < 2; ++port) {
        if (style != brimir::CrosshairStyle::Off && g_portKinds[port] == brimir::PeripheralKind::VirtuaGun &&
            !inputs[port].gunOffscreen) {
            g_crosshairs[g_crosshairCount++] = { style, inputs[port].gunX, inputs[port].gunY,
                                                 brimir::kCrosshairColors[port] };
        }
    }
}

// The software frame with the crosshairs drawn on a copy (the core's frame stays as emulated), or the frame itself.
static std::vector<uint32_t> g_crosshairFrame;

static const void* frame_with_crosshairs(const void* fb, unsigned width, unsigned height, size_t& pitch) {
    if (g_crosshairCount == 0 || fb == nullptr || width == 0 || height == 0) {
        return fb;
    }
    g_crosshairFrame.resize(static_cast<size_t>(width) * height);
    for (unsigned y = 0; y < height; ++y) {
        std::memcpy(g_crosshairFrame.data() + static_cast<size_t>(y) * width,
                    static_cast<const uint8_t*>(fb) + y * pitch, static_cast<size_t>(width) * 4);
    }
    for (unsigned i = 0; i < g_crosshairCount; ++i) {
        brimir::DrawCrosshair(g_crosshairs[i], g_crosshairFrame.data(), width, height, width);
    }
    pitch = static_cast<size_t>(width) * 4;
    return g_crosshairFrame.data();
}

#ifdef BRIMIR_VULKAN
static unsigned requested_internal_resolution(void) {
    const int n = atoi(g_options.internal_resolution.c_str());
    return n < 1 ? 1u : (n > 4 ? 4u : static_cast<unsigned>(n));
}

static void note_internal_resolution(void);

// brimir_vdp1_upscaling (VDP1 upscaling: VDP1 drawn at the internal resolution, GPU stage 3)
static bool vdp1_upscaling_requested(void) {
    return g_options.vdp1_upscaling != "disabled";
}
#endif

// The SH-2 JIT is not used while SH-2 cache emulation is active (the option or the game database): logged once each
// time the two meet (a JIT change, or content loaded with the cache).
static bool g_jitCacheNoted = false;

static void note_jit_with_sh2_cache(void) {
    const bool conflict = g_core && g_core->IsSH2JitEnabled() && g_core->IsSH2CacheEmulationActive();
    if (conflict && !g_jitCacheNoted) {
        brimir_log(RETRO_LOG_INFO, "SH-2 JIT is not used while SH-2 cache emulation is active (option or game "
                                   "database): the SH-2s run in the interpreter");
    }
    g_jitCacheNoted = conflict;
}

static void apply_core_options(bool force) {
    if (!g_core) return;

    auto apply = [force](const char* key, std::string& cached, auto setter) {
        const char* value = get_option_value(key, cached.c_str());
        if (force || cached != value) {
            cached = std::string(value);
            setter(value);
        }
    };

    apply("brimir_audio_interpolation",     g_options.audio_interp,     [](const char* v){ g_core->SetAudioInterpolation(v); });
    apply("brimir_cd_speed",                g_options.cd_speed,         [](const char* v){ g_core->SetCDReadSpeed(static_cast<uint8_t>(atoi(v))); });
    apply("brimir_sh2_overclock",           g_options.sh2_overclock,    [](const char* v){ g_core->SetSH2OverclockFactor(static_cast<uint32_t>(atoi(v))); });
    apply("brimir_autodetect_region",       g_options.autodetect_region,[](const char* v){ g_core->SetAutodetectRegion(strcmp(v, "enabled") == 0); });
    // Both options drive one flag, so it is derived from both values together.
    {
        const char* deinterlacing = get_option_value("brimir_deinterlacing", g_options.deinterlace.deinterlacing.c_str());
        const char* mode = get_option_value("brimir_deinterlace_mode", g_options.deinterlace.mode.c_str());
        bool effective = true;
        if (brimir_update_deinterlace(g_options.deinterlace, deinterlacing, mode, force, effective)) {
            g_core->SetDeinterlacing(effective);
        }
    }
    apply("brimir_audio_volume",            g_options.audio_volume,     [](const char* v){ g_core->SetAudioVolume(atoi(v)); });
    apply("brimir_rotation",                g_options.rotation,         [](const char* v){ g_core->SetRotation(atoi(v)); });
    apply("brimir_overscan",                g_options.overscan,         [](const char* v){
        int overscan = atoi(v);
        g_core->SetOverscanCrop(overscan * 16, overscan * 16);
    });
    apply("brimir_profiling",               g_options.profiling,        [](const char* v){ g_core->SetProfilingEnabled(strcmp(v, "enabled") == 0); });
    apply("brimir_cd_preload",              g_options.cd_preload,       [](const char* v){ g_core->SetDiscPreloadEnabled(strcmp(v, "enabled") == 0); });
    apply("brimir_threaded_vdp1",           g_options.threaded_vdp1,    [](const char* v){ g_core->SetThreadedVDP1(strcmp(v, "enabled") == 0); });
    apply("brimir_threaded_vdp2",           g_options.threaded_vdp2,    [](const char* v){ g_core->SetThreadedVDP2(strcmp(v, "enabled") == 0); });
    apply("brimir_sh2_jit_compile_thread",  g_options.sh2_jit_compile_thread, [](const char* v){ g_core->SetSH2JitCompileThread(strcmp(v, "disabled") != 0); });
    apply("brimir_sh2_jit",                 g_options.sh2_jit,          [](const char* v){ g_core->SetSH2JitEnabled(strcmp(v, "enabled") == 0); });
    apply("brimir_gun_crosshair",           g_options.gun_crosshair,    [](const char*){}); // read per shown frame
    // The three controller options form one setting, applied from all three cached values.
    const auto apply_input_settings = [](const char*) {
        g_core->SetInputSettings(brimir_input_settings(g_options.analog_deadzone.c_str(),
                                                       g_options.trigger_deadzone.c_str(),
                                                       g_options.mouse_sensitivity.c_str()));
    };
    apply("brimir_analog_deadzone",         g_options.analog_deadzone,  apply_input_settings);
    apply("brimir_trigger_deadzone",        g_options.trigger_deadzone, apply_input_settings);
    apply("brimir_mouse_sensitivity",       g_options.mouse_sensitivity, apply_input_settings);
    apply("brimir_scsp_granularity",        g_options.scsp_granularity, [](const char* v){ g_core->SetSCSPStepGranularity(static_cast<uint32_t>(atoi(v))); });

    if (const std::string warning = g_core->TakeSH2JitWorkerWarning(); !warning.empty()) {
        brimir_log(RETRO_LOG_WARN, "%s", warning.c_str());
    }
    note_jit_with_sh2_cache();

    // The RTC mode is chosen at content load (retro_load_game); a live change only takes effect with the next content.
    if (!force) {
        const char* rtc_mode = get_option_value("brimir_rtc_mode", g_options.rtc_mode.c_str());
        if (g_options.rtc_mode != rtc_mode) {
            g_options.rtc_mode = rtc_mode;
            brimir_log(RETRO_LOG_INFO, "RTC mode '%s' applies when content is next loaded", rtc_mode);
        }
        const char* cartridge = get_option_value("brimir_cartridge", g_options.cartridge.c_str());
        if (g_options.cartridge != cartridge) {
            g_options.cartridge = cartridge;
            brimir_log(RETRO_LOG_INFO, "Cartridge '%s' applies when content is next loaded", cartridge);
        }
        const char* sh2_cache = get_option_value("brimir_sh2_cache", g_options.sh2_cache.c_str());
        if (g_options.sh2_cache != sh2_cache) {
            g_options.sh2_cache = sh2_cache;
            brimir_log(RETRO_LOG_INFO, "SH-2 cache emulation '%s' applies when content is next loaded", sh2_cache);
        }
        const char* cdblock_lle = get_option_value("brimir_cdblock_lle", g_options.cdblock_lle.c_str());
        if (g_options.cdblock_lle != cdblock_lle) {
            g_options.cdblock_lle = cdblock_lle;
            brimir_log(RETRO_LOG_INFO, "CD Block LLE '%s' applies when content is next loaded", cdblock_lle);
        }
    }

#ifdef BRIMIR_VULKAN
    // The renderer is chosen at content load; a live change only takes effect with the next content.
    if (!force) {
        const char* renderer = get_option_value("brimir_renderer", g_options.renderer.c_str());
        if (g_options.renderer != renderer) {
            g_options.renderer = renderer;
            brimir_log(RETRO_LOG_INFO, "Renderer '%s' applies when content is next loaded", renderer);
        }
    }
    apply("brimir_internal_resolution", g_options.internal_resolution, [](const char*) {
        if (g_vulkanOutput) {
            g_vulkanOutput->SetInternalScale(requested_internal_resolution());
        }
        note_internal_resolution();
    });
    apply("brimir_vdp1_upscaling", g_options.vdp1_upscaling, [](const char*) {
        if (g_vulkanOutput) {
            g_vulkanOutput->SetVdp1Upscaling(vdp1_upscaling_requested());
        }
    });
#endif
}

#ifdef BRIMIR_VULKAN
static const char* vulkan_mode_name(brimir::libretro::VulkanOutputMode mode) {
    switch (mode) {
    case brimir::libretro::VulkanOutputMode::Compute: return "compute";
    case brimir::libretro::VulkanOutputMode::CopyOnly: return "copy-only";
    default: return "none";
    }
}

// The bridge hands raw frames to the GPU only for the compute output; the copy-only output uploads the bridge's
// own converted frame, so the CPU path stays on. (While the Vulkan VDP renderer is installed the bridge gets no
// software frames at all: its frames stay on the device.)
static void sync_gpu_output(void) {
    const bool compute =
        g_vulkanOutput && g_vulkanOutput->Mode() == brimir::libretro::VulkanOutputMode::Compute;
    if (g_core && g_core->IsGpuOutputActive() != compute) {
        g_core->SetGpuOutputActive(compute);
    }
}

// Diagnostics: with the environment variable BRIMIR_VDP2_PROBE set (brimir_vkhost --probe), the Vulkan VDP renderer
// logs one debug line per VDP2 frame with the front half's feature counters and CPU time.
static void set_vdp2_probe(brimir::gpu::VulkanVDPRenderer& renderer) {
    static const bool requested = [] {
        const char* env = std::getenv("BRIMIR_VDP2_PROBE");
        return env != nullptr && *env != '\0' && std::strcmp(env, "0") != 0;
    }();
    if (!requested) {
        return;
    }
    renderer.FrameProbe = [](const brimir::gpu::vdp2::Vdp2FeatureCounters& f, double frontSeconds,
                             const brimir::gpu::VulkanVDPRenderer::Vdp1ProbeTimes& vdp1) {
        brimir_log(RETRO_LOG_DEBUG,
                   "VDP2 probe: batches %u arena %u copy %u mosaic %u vcell %u chardelay %u ddlinescroll %u "
                   "extcc %u shadow %u mesh %u crammodes %u front_us %.1f vdp1_front_us %.1f vdp1_record_us %.1f "
                   "vdp1_gpu_us %.1f",
                   f.batches, f.arenaBytes, f.copyBytes, f.mosaicLines, f.vcellLines, f.charPatDelayLines,
                   f.ddLineScrollLines, f.extendedCCLines, f.shadowLines, f.meshSlices, f.cramModeMask,
                   frontSeconds * 1e6, vdp1.frontSeconds * 1e6, vdp1.recordSeconds * 1e6, vdp1.gpuSeconds * 1e6);
    };
}

// Diagnostics: with BRIMIR_GPU_PASS_PROFILE set (brimir_vkhost --probe-passes), the GPU time of every pass kind, one
// debug line per VDP2 frame ("GPU passes: ...", VulkanVDPRenderer::PassProfileProbe).
static void set_pass_profile(brimir::gpu::VulkanVDPRenderer& renderer) {
    static const bool requested = [] {
        const char* env = std::getenv("BRIMIR_GPU_PASS_PROFILE");
        return env != nullptr && *env != '\0' && std::strcmp(env, "0") != 0;
    }();
    if (requested) {
        renderer.PassProfileProbe = [](const std::string& line) { brimir_log(RETRO_LOG_DEBUG, "%s", line.c_str()); };
    }
    // Diagnostics: BRIMIR_VDP1_NX_OVERWRITE=0 turns the VDP1 upscaling fast path off (every N x sub-batch ordered)
    const char* overwrite = std::getenv("BRIMIR_VDP1_NX_OVERWRITE");
    if (overwrite != nullptr && std::strcmp(overwrite, "0") == 0) {
        renderer.SetVdp1OverwritePath(false);
        brimir_log(RETRO_LOG_INFO, "VDP1 upscaling: overwrite-class fast path off (BRIMIR_VDP1_NX_OVERWRITE=0)");
    }
}

// Diagnostics: BRIMIR_VDP1_DUMP_OPS="<frame>|<file>" (brimir_vkhost --dump-vdp1-ops) captures the GPU VDP1 work of
// one VDP2 frame, counted from the renderer's attach (VulkanVDPRenderer::SetVdp1OpDump). Once per process.
static void set_vdp1_op_dump(brimir::gpu::VulkanVDPRenderer& renderer) {
    static bool done = false;
    const char* env = std::getenv("BRIMIR_VDP1_DUMP_OPS");
    if (done || env == nullptr) {
        return;
    }
    const std::string text = env;
    const size_t bar = text.find('|');
    if (bar == std::string::npos || bar == 0 || bar + 1 == text.size()) {
        return;
    }
    done = true;
    // Frame F >= 1: the start state is taken at the end of frame F - 1
    const unsigned long long frame = std::strtoull(text.substr(0, bar).c_str(), nullptr, 10);
    if (frame < 1 || frame > 0xFFFFFFFFull) {
        brimir_log(RETRO_LOG_WARN, "VDP1 op dump: frame must be 1 or more (BRIMIR_VDP1_DUMP_OPS=%s)", env);
        return;
    }
    renderer.SetVdp1OpDump(static_cast<uint32_t>(frame), text.substr(bar + 1));
}

// VDP1 runs on the GPU (GPU stage 2) whenever the Vulkan VDP renderer is attached; the debug override
// BRIMIR_VDP1_GPU=0 (brimir_vkhost --software-vdp1) keeps it in software inside the Vulkan VDP renderer, as in stage 1
// (also the fallback when the VDP1 back half cannot be created, and VDP1 while the renderer is detached).
static bool vdp1_mode_requested(void) {
    static const bool requested = [] {
        const char* env = std::getenv("BRIMIR_VDP1_GPU");
        return env == nullptr || std::strcmp(env, "0") != 0;
    }();
    return requested;
}

// The VDP renderer follows the frame output: the Vulkan VDP renderer while the compute output is live, the software
// renderer otherwise (copy-only output, no context, or the renderer cannot run on the device). Switching carries the
// renderer state over (CoreWrapper). Call between frames: context_reset, the start of retro_run, or after a frame
// output change.
static void sync_vdp_renderer(void) {
    if (!g_core) {
        return;
    }
    using brimir::libretro::VulkanOutputMode;
    const bool compute = g_vulkanRequested && g_vulkanOutput && g_vulkanOutput->Mode() == VulkanOutputMode::Compute;
    if (compute && !g_vulkanRendererFailed && !g_vulkanOutput->RendererAttached()) {
        std::string error;
        brimir::gpu::VulkanVDPRenderer* renderer = g_core->InstallVulkanRenderer();
        if (renderer != nullptr) {
            // Allocation failures (internal resolution) and the effective scale after a fallback.
            renderer->SetLog([](const std::string& text) { brimir_log(RETRO_LOG_WARN, "%s", text.c_str()); });
        }
        if (renderer == nullptr) {
            error = "installing it failed";
        } else {
            renderer->SetVdp1Mode(vdp1_mode_requested() ? brimir::gpu::Vdp1Mode::Gpu : brimir::gpu::Vdp1Mode::Software);
            set_pass_profile(*renderer); // before the attach: the profile starts with Gpu VDP1 mode
            if (g_vulkanOutput->AttachRenderer(*renderer, error)) {
                set_vdp2_probe(*renderer);
                set_vdp1_op_dump(*renderer);
                brimir_log(RETRO_LOG_INFO, "Vulkan VDP2 renderer active");
                if (renderer->GpuVdp1()) {
                    brimir_log(RETRO_LOG_INFO, "Vulkan VDP1 renderer active (VDP1 on the GPU)");
                } else if (vdp1_mode_requested()) {
                    brimir_log(RETRO_LOG_WARN, "VDP1 on the GPU unavailable; VDP1 in software");
                }
            }
        }
        if (!g_vulkanOutput->RendererAttached()) {
            g_vulkanRendererFailed = true;
            g_core->UseSoftwareRenderer();
            brimir_log(RETRO_LOG_WARN, "Vulkan VDP2 renderer unavailable (%s); software renderer with the GPU frame output",
                       error.c_str());
        }
    } else if (!(compute && g_vulkanOutput->RendererAttached()) && g_core->GetVulkanRenderer()) {
        std::string error;
        if (g_vulkanOutput && !g_vulkanOutput->DetachRenderer(error)) {
            brimir_log(RETRO_LOG_ERROR, "Vulkan VDP2 renderer detach: %s", error.c_str());
        }
        g_core->UseSoftwareRenderer();
        brimir_log(RETRO_LOG_INFO, "Vulkan VDP2 renderer off (frame output %s); software renderer",
                   g_vulkanOutput ? vulkan_mode_name(g_vulkanOutput->Mode()) : "none");
    }
    sync_gpu_output();
}

// brimir_internal_resolution above 1 only applies with the Vulkan VDP2 renderer on the compute output: with the
// software renderer, the copy-only output or a renderer that could not be installed the frames stay native (said once
// per content). Unknown (and silent) between the Vulkan request and the first context_reset.
static void note_internal_resolution(void) {
    const unsigned n = requested_internal_resolution();
    if (n <= 1 || g_internalResolutionNoopLogged || g_loadingContent) {
        return;
    }
    using brimir::libretro::VulkanOutputMode;
    const char* reason = nullptr;
    if (!g_vulkanRequested) {
        reason = "the software renderer";
    } else if (g_vulkanOutput && g_vulkanOutput->Live() && g_vulkanOutput->Mode() != VulkanOutputMode::Compute) {
        reason = "the copy-only frame output";
    } else if (g_vulkanRendererFailed) {
        reason = "the software renderer (the Vulkan VDP2 renderer is unavailable)";
    }
    if (reason != nullptr) {
        g_internalResolutionNoopLogged = true;
        brimir_log(RETRO_LOG_INFO, "Internal resolution %ux needs the Vulkan VDP2 renderer; frames stay native with %s",
                   n, reason);
    }
}

static void RETRO_CALLCONV vulkan_context_reset(void) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_vulkanRequested) {
        return;
    }
    GpuWorkScope gpuWork;
    if (!g_vulkanOutput) {
        g_vulkanOutput = std::make_unique<brimir::libretro::VulkanOutput>();
        GpuWorkScope::Apply();
    }
    g_vulkanOutput->SetInternalScale(requested_internal_resolution());
    g_vulkanOutput->SetVdp1Upscaling(vdp1_upscaling_requested());
    std::string reason;
    const bool live = g_vulkanOutput->ContextReset(environ_cb, reason);
    g_vulkanPresentErrorLogged = false;
    g_vulkanRendererFailed = false;
    if (!live) {
        // No usable device at all, and too late to fall back (the frontend runs its Vulkan driver now): keep
        // handing over software frames, which RetroArch's vulkan driver does not show.
        brimir_log(RETRO_LOG_ERROR, "Vulkan context unusable, presenting software frames: %s", reason.c_str());
        if (environ_cb) {
            static std::string text;
            text = "Brimir: Vulkan renderer unavailable (" + reason.substr(0, 120) +
                   "). Set Renderer to Software or change the video driver.";
            retro_message message{text.c_str(), 600};
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
        }
    } else if (g_vulkanOutput->Mode() == brimir::libretro::VulkanOutputMode::CopyOnly) {
        brimir_log(RETRO_LOG_WARN, "Vulkan compute frame output unavailable (%s); using copy-only frame output",
                   reason.c_str());
        if (environ_cb) {
            static std::string text;
            // The reason names the cause: the GPU (version, limits, queue) or the frontend (device not negotiated).
            text = "Brimir: GPU renderer unavailable (" + reason.substr(0, 120) + "); using the software renderer.";
            retro_message message{text.c_str(), 600};
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
        }
    } else {
        brimir_log(RETRO_LOG_INFO, "Vulkan frame output ready");
    }
    // Install (first usable context) or attach the Vulkan VDP renderer; revert to software without the compute output.
    sync_vdp_renderer();
    note_internal_resolution();
}

static void RETRO_CALLCONV vulkan_context_destroy(void) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    // Waits for the device, detaches the Vulkan VDP renderer (it stays installed for the next context_reset), then
    // frees the output. Safe between VDP2EndFrame and the frame's recording (a reinit inside retro_run).
    GpuWorkScope gpuWork;
    if (g_vulkanOutput) {
        g_vulkanOutput->ContextDestroy();
    }
    sync_gpu_output();
}

static void log_vulkan_output_change(brimir::libretro::VulkanOutputMode before, const std::string& error) {
    if (g_vulkanOutput->Mode() != before) {
        brimir_log(RETRO_LOG_WARN, "Vulkan frame output changed to %s: %s", vulkan_mode_name(g_vulkanOutput->Mode()),
                   error.c_str());
    } else if (!error.empty() && !g_vulkanPresentErrorLogged) {
        g_vulkanPresentErrorLogged = true;
        brimir_log(RETRO_LOG_ERROR, "Vulkan frame output failed: %s", error.c_str());
    }
}

// Before RunFrame: the VDP renderer follows the output (a context change since the last frame). A sync index mask
// growth is handled by the next shown frame's BeginFrame, which keeps the renderer's composite.
static void prepare_vulkan_frame(void) {
    sync_vdp_renderer();
    if (g_vulkanOutput) {
        g_vulkanOutput->BeginRun();
    }
}

// The Vulkan half of retro_run's video output: present the frame RunFrame completed.
static void present_vulkan_frame(void) {
    using brimir::libretro::VulkanOutputMode;

    // Run-ahead and fast-forward frames the frontend will not show: no presenter, no set_image, no video_cb. The
    // Vulkan VDP renderer still submits the frame's VDP2 work (its VRAM copy and, when weaving, the other field
    // must stay current).
    int av = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
    const bool hidden =
        environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av) && !(av & RETRO_AV_ENABLE_VIDEO);
    if (!g_vulkanOutput->Live()) {
        // Destroyed by a reinit inside the call above, without a new context: nothing to present, repeat.
        if (!hidden && video_cb) {
            video_cb(nullptr, g_lastWidth, g_lastHeight, 0);
        }
        return;
    }
    const VulkanOutputMode mode = g_vulkanOutput->Mode();
    const bool renderer = mode == VulkanOutputMode::Compute && g_vulkanOutput->RendererAttached();
    if (hidden || !video_cb) {
        std::string error;
        if (renderer && !g_vulkanOutput->SubmitHidden(error) && !g_vulkanPresentErrorLogged) {
            g_vulkanPresentErrorLogged = true;
            brimir_log(RETRO_LOG_ERROR, "Vulkan frame output failed (hidden frame): %s", error.c_str());
        }
        return;
    }

    unsigned width = 0;
    unsigned height = 0;
    std::string error;
    bool presented = false;
    g_vulkanOutput->SetCrosshairs(std::span<const brimir::Crosshair>(g_crosshairs, g_crosshairCount));
    if (renderer) {
        // The renderer's frame of this run, or (no VDP2 frame ended) the composite again at the last size.
        const brimir::CoreWrapper::DeviceFrame& frame = g_core->LastDeviceFrame();
        if (frame.width != 0) {
            const brimir::FrameOutputSettings& s = frame.settings;
            presented = g_vulkanOutput->PresentRenderer(frame.width, frame.height, {s.cropH, s.cropV, s.rotation},
                                                        width, height, error);
        }
    } else if (mode == VulkanOutputMode::Compute) {
        if (const uint32_t* raw = g_core->RawFrame()) {
            const brimir::FrameOutputSettings& s = g_core->RawFrameSettings();
            presented = g_vulkanOutput->Present(raw, g_core->RawFrameWidth(), g_core->RawFrameHeight(),
                                                {s.cropH, s.cropV, s.rotation}, width, height, error);
        }
    } else if (g_core->CpuFrameFromLastRun()) {
        // Only a frame this run completed: after a switch from the Vulkan VDP renderer (a reinit ending in copy-only)
        // the bridge's frame predates the renderer.
        width = g_core->GetFramebufferWidth();
        height = g_core->GetFramebufferHeight();
        presented = g_vulkanOutput->PresentCopy(g_core->GetFramebuffer(), width, height,
                                                g_core->GetFramebufferPitch(), error);
    }
    if (renderer && g_vulkanOutput->RendererAttached()) {
        // VDP1 on the GPU: ops the present did not take (nothing to show yet, or no present) go in the ops-only
        // submit, so none waits past this retro_run.
        brimir::gpu::VulkanVDPRenderer* vulkanRenderer = g_core->GetVulkanRenderer();
        std::string opsError;
        if (vulkanRenderer && vulkanRenderer->HasPendingVdp1Work() && !g_vulkanOutput->SubmitHidden(opsError) &&
            !g_vulkanPresentErrorLogged) {
            g_vulkanPresentErrorLogged = true;
            brimir_log(RETRO_LOG_ERROR, "Vulkan frame output failed (VDP1 ops): %s", opsError.c_str());
        }
    }

    if (!presented) {
        log_vulkan_output_change(mode, error);
        if (g_vulkanOutput->Mode() != mode || (renderer && !g_vulkanOutput->RendererAttached())) {
            sync_vdp_renderer();
        }
        // Nothing new to show: repeat the last frame.
        video_cb(nullptr, g_lastWidth, g_lastHeight, 0);
        return;
    }

    // The scale of the frames handed over (brimir_vkhost --hash-sample and --sharpness follow this line), logged when it
    // changes: N x the native size with the Vulkan VDP2 renderer, native otherwise.
    if (const unsigned scale = g_vulkanOutput->PresentedScale(); scale != g_presentedScale) {
        g_presentedScale = scale;
        brimir_log(RETRO_LOG_INFO, "Internal resolution: frames at %ux (requested %ux)", scale,
                   requested_internal_resolution());
    }
    // A change of the internal resolution is a base-size change: N x the native output, within the maximum declared at
    // load (SET_GEOMETRY only, never SET_SYSTEM_AV_INFO).
    if (environ_cb && (width != g_lastWidth || height != g_lastHeight)) {
        struct retro_game_geometry geo = {};
        geo.base_width = width;
        geo.base_height = height;
        geo.max_width = max_geometry_width();
        geo.max_height = max_geometry_height();
        geo.aspect_ratio = 4.0f / 3.0f;
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);
        g_lastWidth = width;
        g_lastHeight = height;
    }
    video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
}
#endif

static const struct retro_controller_description kPortTypes[] = {
    { "Control Pad", brimir::RETRO_DEVICE_SATURN_PAD },
    { "3D Control Pad", brimir::RETRO_DEVICE_SATURN_3D_PAD },
    { "Arcade Racer", brimir::RETRO_DEVICE_SATURN_WHEEL },
    { "Mission Stick", brimir::RETRO_DEVICE_SATURN_MISSION_STICK },
    { "Shuttle Mouse", brimir::RETRO_DEVICE_SATURN_MOUSE },
    { "Virtua Gun", brimir::RETRO_DEVICE_SATURN_VIRTUA_GUN },
    { "None", RETRO_DEVICE_NONE },
};
static const struct retro_controller_info kControllerInfo[] = {
    { kPortTypes, sizeof(kPortTypes) / sizeof(kPortTypes[0]) },
    { kPortTypes, sizeof(kPortTypes) / sizeof(kPortTypes[0]) },
    { nullptr, 0 },
};

// Input descriptors of both ports' device types (the layouts of brimir/peripheral_input.hpp). The Control Pad's are
// Brimir's original ones:
// RetroPad B  -> Saturn A
// RetroPad A  -> Saturn B
// RetroPad Y  -> Saturn C
// RetroPad X  -> Saturn X
// RetroPad L2 -> Saturn Y
// RetroPad R2 -> Saturn Z
// RetroPad L  -> Saturn L (shoulder)
// RetroPad R  -> Saturn R (shoulder)
static void set_input_descriptors(retro_environment_t cb) {
    static std::vector<retro_input_descriptor> descriptors;
    descriptors.clear();
    for (unsigned port = 0; port < 2; ++port) {
        const auto pad = [&](unsigned id, const char* text) {
            descriptors.push_back({ port, RETRO_DEVICE_JOYPAD, 0, id, text });
        };
        const auto stick = [&](unsigned index, unsigned id, const char* text) {
            descriptors.push_back({ port, RETRO_DEVICE_ANALOG, index, id, text });
        };
        const auto faceButtons = [&] { // 3D Control Pad, Arcade Racer, Mission Stick (Beetle Saturn's layout)
            pad(RETRO_DEVICE_ID_JOYPAD_B, "A");
            pad(RETRO_DEVICE_ID_JOYPAD_A, "B");
            pad(RETRO_DEVICE_ID_JOYPAD_R, "C");
            pad(RETRO_DEVICE_ID_JOYPAD_Y, "X");
            pad(RETRO_DEVICE_ID_JOYPAD_X, "Y");
            pad(RETRO_DEVICE_ID_JOYPAD_L, "Z");
        };
        switch (g_portKinds[port]) {
        case brimir::PeripheralKind::None: break;
        case brimir::PeripheralKind::ControlPad:
            pad(RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
            pad(RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
            pad(RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
            pad(RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
            pad(RETRO_DEVICE_ID_JOYPAD_B, "A");
            pad(RETRO_DEVICE_ID_JOYPAD_A, "B");
            pad(RETRO_DEVICE_ID_JOYPAD_Y, "C");
            pad(RETRO_DEVICE_ID_JOYPAD_X, "X");
            pad(RETRO_DEVICE_ID_JOYPAD_L2, "Y");
            pad(RETRO_DEVICE_ID_JOYPAD_R2, "Z");
            pad(RETRO_DEVICE_ID_JOYPAD_L, "L");
            pad(RETRO_DEVICE_ID_JOYPAD_R, "R");
            pad(RETRO_DEVICE_ID_JOYPAD_START, "Start");
            break;
        case brimir::PeripheralKind::AnalogPad:
            pad(RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
            pad(RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
            pad(RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
            pad(RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
            faceButtons();
            pad(RETRO_DEVICE_ID_JOYPAD_L2, "L (analog)");
            pad(RETRO_DEVICE_ID_JOYPAD_R2, "R (analog)");
            pad(RETRO_DEVICE_ID_JOYPAD_START, "Start");
            pad(RETRO_DEVICE_ID_JOYPAD_SELECT, "Mode Switch");
            stick(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X");
            stick(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y");
            break;
        case brimir::PeripheralKind::ArcadeRacer:
            faceButtons();
            pad(RETRO_DEVICE_ID_JOYPAD_L2, "Shift Paddle Up");
            pad(RETRO_DEVICE_ID_JOYPAD_R2, "Shift Paddle Down");
            pad(RETRO_DEVICE_ID_JOYPAD_UP, "Shift Up");
            pad(RETRO_DEVICE_ID_JOYPAD_DOWN, "Shift Down");
            pad(RETRO_DEVICE_ID_JOYPAD_START, "Start");
            stick(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Wheel");
            break;
        case brimir::PeripheralKind::MissionStick:
            faceButtons();
            pad(RETRO_DEVICE_ID_JOYPAD_L2, "L");
            pad(RETRO_DEVICE_ID_JOYPAD_R2, "R");
            pad(RETRO_DEVICE_ID_JOYPAD_START, "Start");
            pad(RETRO_DEVICE_ID_JOYPAD_R3, "Throttle Latch");
            stick(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Stick X");
            stick(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Stick Y");
            stick(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Throttle");
            break;
        case brimir::PeripheralKind::ShuttleMouse: break; // mouse buttons and movement have no descriptors
        case brimir::PeripheralKind::VirtuaGun:
            descriptors.push_back({ port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, "Trigger" });
            descriptors.push_back({ port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD, "Reload" });
            descriptors.push_back({ port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START, "Start" });
            break;
        }
    }
    descriptors.push_back({ 0, 0, 0, 0, nullptr });
    if (cb) {
        cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors.data());
    }
}

// The RetroPad buttons of a port: one call with RETRO_DEVICE_ID_JOYPAD_MASK when the frontend supports it, otherwise
// one per button the port's device uses (the sticks' buttons only for the analog devices).
static uint16_t poll_joypad(unsigned port, bool sticks) {
    if (g_input_bitmask_supported) {
        return static_cast<uint16_t>(input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
    }
    static const unsigned kButtons[] = {
        RETRO_DEVICE_ID_JOYPAD_B,     RETRO_DEVICE_ID_JOYPAD_Y,    RETRO_DEVICE_ID_JOYPAD_SELECT,
        RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_UP,   RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_LEFT,  RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_A,
        RETRO_DEVICE_ID_JOYPAD_X,     RETRO_DEVICE_ID_JOYPAD_L,    RETRO_DEVICE_ID_JOYPAD_R,
        RETRO_DEVICE_ID_JOYPAD_L2,    RETRO_DEVICE_ID_JOYPAD_R2,
    };
    uint16_t buttons = 0;
    for (const unsigned id : kButtons) {
        if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id)) {
            buttons |= static_cast<uint16_t>(1u << id);
        }
    }
    if (sticks) {
        for (const unsigned id : { RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3 }) {
            if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id)) {
                buttons |= static_cast<uint16_t>(1u << id);
            }
        }
    }
    return buttons;
}

// This frame's input of a port, as its device type needs it.
static brimir::InputSnapshot poll_port(unsigned port, brimir::PeripheralKind kind) {
    using brimir::PeripheralKind;
    brimir::InputSnapshot in;
    switch (kind) {
    case PeripheralKind::None: break;
    case PeripheralKind::ControlPad: in.joypad = poll_joypad(port, false); break;
    case PeripheralKind::AnalogPad:
    case PeripheralKind::ArcadeRacer:
    case PeripheralKind::MissionStick:
        in.joypad = poll_joypad(port, true);
        in.leftX = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
        in.leftY = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
        in.rightX = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
        in.rightY = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
        in.l2 = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2);
        in.r2 = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2);
        break;
    case PeripheralKind::ShuttleMouse: {
        using S = brimir::InputSnapshot;
        in.mouseX = input_state_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
        in.mouseY = input_state_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
        const struct {
            unsigned id;
            uint8_t bit;
        } kMouseButtons[] = {
            { RETRO_DEVICE_ID_MOUSE_LEFT, S::kMouseLeft },         { RETRO_DEVICE_ID_MOUSE_RIGHT, S::kMouseRight },
            { RETRO_DEVICE_ID_MOUSE_MIDDLE, S::kMouseMiddle },     { RETRO_DEVICE_ID_MOUSE_BUTTON_4, S::kMouseButton4 },
            { RETRO_DEVICE_ID_MOUSE_BUTTON_5, S::kMouseButton5 },
        };
        for (const auto& b : kMouseButtons) {
            if (input_state_cb(port, RETRO_DEVICE_MOUSE, 0, b.id)) {
                in.mouseButtons |= b.bit;
            }
        }
        break;
    }
    case PeripheralKind::VirtuaGun:
        in.gunX = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
        in.gunY = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
        in.gunOffscreen = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) != 0;
        in.gunTrigger = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) != 0;
        in.gunReload = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD) != 0;
        in.gunStart = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START) != 0;
        break;
    }
    return in;
}

// Libretro API implementation

RETRO_API void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    
    // Set core options v2
    unsigned version = 0;
    if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
        const struct retro_core_options_v2* options_v2 = brimir_get_core_options_v2();
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, (void*)options_v2);
    }
    
    // Set core options
    bool no_content = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);
    
    // Input descriptors of the ports' device types (the Control Pad's until the frontend chooses others), and the
    // device types each port offers.
    set_input_descriptors(cb);
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)kControllerInfo);
    
    // Request log interface
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
    }

    // Check for bitmask input support (avoids 14 separate calls per player per frame)
    g_input_bitmask_supported = cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) {
    video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) {
    audio_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb) {
    input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb) {
    input_state_cb = cb;
}

RETRO_API void retro_init(void) {
    brimir_log(RETRO_LOG_INFO, "Brimir initializing...");
    brimir_log(RETRO_LOG_INFO, "Based on Ymir emulator by StrikerX3");
    
    {
        std::lock_guard<std::recursive_mutex> lock(g_coreMutex);

        // Create core instance
        g_core = std::make_unique<brimir::CoreWrapper>();

        if (!g_core->Initialize()) {
            brimir_log(RETRO_LOG_ERROR, "Failed to initialize core");
            g_core.reset();
            return;
        }
        for (unsigned port = 0; port < 2; ++port) {
            g_core->SetPeripheral(port, g_portKinds[port]);
        }
    }

    // Register disk control interface for multi-disc games
    unsigned dc_version = 0;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &dc_version) && dc_version >= 1) {
        static struct retro_disk_control_ext_callback disk_cb = {
            disk_set_eject_state,
            disk_get_eject_state,
            disk_get_image_index,
            disk_set_image_index,
            disk_get_num_images,
            disk_replace_image_index,
            disk_add_image_index,
            disk_set_initial_image,
            disk_get_image_path,
            disk_get_image_label,
        };
        environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_cb);
        brimir_log(RETRO_LOG_INFO, "Disk control ext interface registered");
    }
}

RETRO_API void retro_deinit(void) {
    brimir_log(RETRO_LOG_INFO, "Brimir shutting down");

    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
#ifdef BRIMIR_VULKAN
    if (g_vulkanOutput) {
        if (g_vulkanOutput->Live()) {
            // The frontend never called context_destroy; its device may be gone, so make no Vulkan call.
            brimir_log(RETRO_LOG_WARN, "Vulkan context still live at deinit; releasing it without cleanup");
        }
        g_vulkanOutput->Abandon();
        g_vulkanOutput.reset();
    }
    g_vulkanRequested = false;
#endif
    if (g_core) {
        // Same rule as retro_unload_game: never let an exception reach the frontend.
        try {
            g_core->Shutdown();
        } catch (const std::exception& e) {
            brimir_log(RETRO_LOG_ERROR, "Exception while shutting down: %s", e.what());
        } catch (...) {
            brimir_log(RETRO_LOG_ERROR, "Unknown exception while shutting down");
        }
        g_core.reset();
    }
}

RETRO_API unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "Brimir";
    info->library_version = BRIMIR_VERSION;
    info->need_fullpath = true;
    // Only formats Ymir's loader can open directly. Track data files (.bin, .img,
    // .mdf) must be loaded through their .cue/.ccd/.mds sheet.
    // Keep in sync with supported_extensions in resources/info/brimir_libretro.info.
    info->valid_extensions = "chd|cue|ccd|mds|iso|m3u";
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    memset(info, 0, sizeof(*info));
    
    // Base resolution for Saturn (varies between games)
    info->geometry.base_width = 320;
    info->geometry.base_height = 224;
    
    // Saturn max native resolution
    info->geometry.max_width = max_geometry_width();
    info->geometry.max_height = max_geometry_height();
    info->geometry.aspect_ratio = 4.0f / 3.0f;

    // Fallback timing. The actual regional framerate (NTSC/PAL) is set once the
    // disc region is known via update_system_av_info_for_region() in retro_load_game.
    info->timing.fps = 59.94;
    info->timing.sample_rate = 44100.0;
}

static void update_system_av_info_for_region(void) {
    if (!environ_cb || !g_core) return;

    // retro_get_system_av_info() already reports 59.94 fps as the fallback.
    // Only push a new system av info if the loaded region is actually PAL,
    // to avoid an unnecessary video driver re-init on NTSC content.
    if (g_core->GetConsoleRegion() != brimir::ConsoleRegion::PAL) {
        return;
    }

    struct retro_system_av_info avi;
    retro_get_system_av_info(&avi);
    avi.timing.fps = 50.0;
    environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &avi);
}

// Between frames (the frontend never calls it during retro_run); a state saved with another device loads with this one
// (each port keeps the state of the peripheral it has).
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (port > 1) {
        brimir_log(RETRO_LOG_DEBUG, "Controller port %u ignored (the Saturn has two)", port + 1);
        return;
    }
    const brimir::PeripheralKind kind = brimir::PeripheralKindForDevice(device);
    brimir_log(RETRO_LOG_INFO, "Controller port %u: %s (device %u)", port + 1, brimir::PeripheralKindName(kind), device);
    if (kind == g_portKinds[port]) {
        return;
    }
    g_portKinds[port] = kind;
    if (g_core) {
        g_core->SetPeripheral(port, kind);
    }
    set_input_descriptors(environ_cb);
}

RETRO_API void retro_reset(void) {
    brimir_log(RETRO_LOG_INFO, "Reset requested");

    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (g_core) {
        g_core->Reset();
        publish_ram_view();
    }
}

// Diagnostics: in builds configured with BRIMIR_VDP1_PROBE=ON and with the environment variable BRIMIR_VDP1_PROBE set
// (brimir_vkhost --probe), one debug line per retro_run with the VDP1 work counted by the software renderer and the draw
// batches the GPU VDP1 front half closed, by end reason (ymir/hw/vdp/renderer/vdp1_probe.hpp). Other builds compile this
// to nothing.
static void log_vdp1_probe(void) {
#if BRIMIR_VDP1_PROBE
    static const bool requested = [] {
        const char* env = std::getenv("BRIMIR_VDP1_PROBE");
        return env != nullptr && *env != '\0' && std::strcmp(env, "0") != 0;
    }();
    const ymir::vdp::probe::VDP1Snapshot s = ymir::vdp::probe::VDP1ProbeTake();
    if (requested) {
        brimir_log(RETRO_LOG_DEBUG,
                   "VDP1 probe: commands %llu lines %llu pixels %llu syncs %llu dirtysyncs %llu fbwrites %llu "
                   "erases %llu batches opkind %llu hazard %llu swap %llu capacity %llu boundary %llu take %llu "
                   "nx rect %llu quad %llu replicate %llu slots %llu subbatches %llu gen_ns %llu",
                   static_cast<unsigned long long>(s.commands), static_cast<unsigned long long>(s.lines),
                   static_cast<unsigned long long>(s.pixels), static_cast<unsigned long long>(s.syncs),
                   static_cast<unsigned long long>(s.dirtySyncs), static_cast<unsigned long long>(s.fbWrites),
                   static_cast<unsigned long long>(s.erases), static_cast<unsigned long long>(s.batchEnds[0]),
                   static_cast<unsigned long long>(s.batchEnds[1]), static_cast<unsigned long long>(s.batchEnds[2]),
                   static_cast<unsigned long long>(s.batchEnds[3]), static_cast<unsigned long long>(s.batchEnds[4]),
                   static_cast<unsigned long long>(s.batchEnds[5]), static_cast<unsigned long long>(s.nxSpans[0]),
                   static_cast<unsigned long long>(s.nxSpans[1]), static_cast<unsigned long long>(s.nxSpans[2]),
                   static_cast<unsigned long long>(s.nxSlots), static_cast<unsigned long long>(s.nxSubBatches),
                   static_cast<unsigned long long>(s.nxGenerateNanos));
    }
#endif
}

// Periodic save of the backup cartridge (<game>.bcr; also written at unload, as is a RAM cartridge's .cart): every
// kCartridgeFlushFrames shown frames (about 5 s), when the cartridge changed. Never on run-ahead's hidden frames nor on
// the frame right after a state load; it only reads the cartridge, so emulation is unaffected.
static constexpr unsigned kCartridgeFlushFrames = 300;
static unsigned g_cartridgeFlushFrames = 0;
static bool g_stateLoadedBeforeRun = false;

static void log_cartridge_save_error(void) {
    if (const std::string error = g_core->TakeCartridgeSaveError(); !error.empty()) {
        brimir_log(RETRO_LOG_WARN, "%s", error.c_str());
    }
}

static void flush_cartridge_save(void) {
    const bool afterLoad = std::exchange(g_stateLoadedBeforeRun, false);
    int av = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
    const bool hidden =
        environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av) && !(av & RETRO_AV_ENABLE_VIDEO);
    if (hidden) {
        return;
    }
    if (++g_cartridgeFlushFrames < kCartridgeFlushFrames || afterLoad) {
        return;
    }
    g_cartridgeFlushFrames = 0;
    if (!g_core->FlushCartridgeSave()) {
        log_cartridge_save_error();
    }
}

RETRO_API void retro_run(void) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) {
        return;
    }
#ifdef BRIMIR_VULKAN
    GpuWorkScope gpuWork;
#endif

    // Only re-query core options when the frontend reports a change. This avoids
    // string comparisons and setter calls on every frame when nothing changed.
    bool updated = false;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        apply_core_options(false);
    }

    // Performance profiling: dump report every 300 frames if enabled
    if (g_options.profiling == "enabled") {
        g_profileFrameCount++;
        if (g_profileFrameCount == 300) {
            brimir_log(RETRO_LOG_INFO, "=== Performance Profile (300 frames) ===");
            std::string report = g_core->GetProfilingReport();
            size_t pos = 0;
            while (pos < report.size()) {
                size_t end = report.find('\n', pos);
                if (end == std::string::npos) end = report.size();
                std::string line = report.substr(pos, end - pos);
                if (!line.empty()) {
                    brimir_log(RETRO_LOG_INFO, "%s", line.c_str());
                }
                pos = end + 1;
            }
            g_core->ResetProfiling();
            g_profileFrameCount = 0;
        }
    } else {
        g_profileFrameCount = 0;
    }

    // Poll input
    if (input_poll_cb) {
        input_poll_cb();
    }
    
    // This frame's input of both ports, as each port's device type needs it (brimir/peripheral_input.hpp)
    if (input_state_cb) {
        brimir::InputSnapshot inputs[2];
        for (unsigned port = 0; port < 2; ++port) {
            inputs[port] = poll_port(port, g_portKinds[port]);
            g_core->SetPeripheralInput(port, inputs[port]);
        }
        update_crosshairs(inputs);
    }
    
#ifdef BRIMIR_VULKAN
    prepare_vulkan_frame();
#endif

    // Frontend writes to the work RAM view, then the cheats, then one frame of emulation, then the view shows the new
    // work RAM. The same point every frame, hidden run-ahead frames included.
    {
        brimir::ScopedTimer timer(g_core->GetProfiler(), "RamView_ApplyFrontendWrites");
        apply_ram_view_writes();
    }
    {
        brimir::ScopedTimer timer(g_core->GetProfiler(), "Cheats_Apply");
        apply_cheats();
    }
    g_core->RunFrame();
    {
        brimir::ScopedTimer timer(g_core->GetProfiler(), "RamView_Publish");
        publish_ram_view();
    }
    log_vdp1_probe();
    
    // Output video
#ifdef BRIMIR_VULKAN
    if (g_vulkanOutput && g_vulkanOutput->Live()) {
        present_vulkan_frame();
    } else
#endif
    if (video_cb) {
        const void* fb = g_core->GetFramebuffer();
        unsigned int width = g_core->GetFramebufferWidth();
        unsigned int height = g_core->GetFramebufferHeight();
        unsigned int pitch = g_core->GetFramebufferPitch();
        
        // Update geometry whenever dimensions change, including the first frame.
        // retro_get_system_av_info() reports a fixed fallback base size, so we
        // correct it here as soon as the running content produces its first frame.
        if (environ_cb && (width != g_lastWidth || height != g_lastHeight)) {
            struct retro_game_geometry geo = {};
            geo.base_width = width;
            geo.base_height = height;
            geo.max_width = max_geometry_width();
            geo.max_height = max_geometry_height();
            geo.aspect_ratio = 4.0f / 3.0f;
            environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);
            g_lastWidth = width;
            g_lastHeight = height;
        }
        
        size_t shownPitch = pitch;
        const void* shown = frame_with_crosshairs(fb, width, height, shownPitch);
        video_cb(shown, width, height, shownPitch);
    }
    
    // Output audio
    if (audio_batch_cb) {
        int16_t audio_buffer[2048 * 2]; // Stereo buffer
        size_t samples = g_core->GetAudioSamples(audio_buffer, 2048);
        if (samples > 0) {
            audio_batch_cb(audio_buffer, samples);
        }
    }

    flush_cartridge_save();
}

RETRO_API size_t retro_serialize_size(void) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) {
        return 0;
    }
    return g_core->GetStateSize();
}

RETRO_API bool retro_serialize(void* data, size_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) {
        return false;
    }
    return g_core->SaveState(data, size);
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) {
        return false;
    }
    const bool wasLLE = g_core->IsCDBlockLLEActive();
    const bool loaded = g_core->LoadState(data, size);
    // Also after a rejected state: the view must match work RAM whatever the load left in it.
    publish_ram_view();
    g_stateLoadedBeforeRun = true;
    if (loaded) {
        if (g_core->IsCDBlockLLEActive() != wasLLE) {
            brimir_log(RETRO_LOG_INFO, "Save state uses %s CD Block emulation: switched to it for this session",
                       wasLLE ? "high-level" : "low-level");
            if (environ_cb) {
                static std::string text;
                text = std::string("Brimir: the save state uses ") + (wasLLE ? "high-level" : "low-level") +
                       " CD Block emulation; switched to it until the content is reloaded";
                retro_message message{text.c_str(), 300};
                environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
            }
        }
        if (const std::string warning = g_core->TakeStateWarningToReport(); !warning.empty()) {
            brimir_log(RETRO_LOG_WARN, "Save state: %s (emulation may differ from the saving session; logged once)",
                       warning.c_str());
        }
        return true;
    }
    if (!g_core->GetLastStateError().empty()) {
        brimir_log(RETRO_LOG_WARN, "Save state rejected: %s", g_core->GetLastStateError().c_str());
    }
    return false;
}

RETRO_API void retro_cheat_reset(void) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    g_cheats.Reset();
}

// Saturn Pro Action Replay codes as in RetroArch's Saturn database (brimir/cheat_engine.hpp). RetroArch-handled
// entries (address/value fields) do not come here: RetroArch writes them through the memory map (the RAM view).
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    const char* text = code != nullptr ? code : "";
    std::optional<std::vector<brimir::CheatOp>> ops = brimir::ParseCheat(text);
    if (!ops) {
        g_cheats.Erase(index);
        brimir_log(RETRO_LOG_WARN, "Cheat %u: '%s' is not an Action Replay code (1AAAAAAA VVVV, 3AAAAAAA 00VV, "
                                   "DAAAAAAA VVVV, sub-codes joined with '+'); ignored", index, text);
        return;
    }
    unsigned master = 0;
    unsigned unknown = 0;
    unsigned malformed = 0;
    unsigned outside = 0;
    unsigned untyped = 0;
    for (const brimir::CheatOp& op : *ops) {
        if (op.kind != brimir::CheatOp::Kind::Ignored) {
            outside += brimir::CheatAddressInWorkRam(op.address) ? 0 : 1;
            untyped += op.noTypeDigit ? 1 : 0;
        } else if (op.reason == brimir::CheatOp::IgnoreReason::MasterCode) {
            ++master;
        } else if (op.reason == brimir::CheatOp::IgnoreReason::UnknownType) {
            ++unknown;
        } else {
            ++malformed;
        }
    }
    const size_t count = ops->size();
    g_cheats.Set(index, enabled, std::move(*ops));
    if (untyped > 0) {
        brimir_log(RETRO_LOG_INFO,
                   "Cheat %u: %u sub-codes have no type digit (0AAAAAAA VVVV); written as 16-bit writes (type 1)",
                   index, untyped);
    }
    if (malformed > 0) {
        brimir_log(RETRO_LOG_WARN,
                   "Cheat %u: '%s': %u of %zu sub-codes malformed (template values such as 00?? must be edited); "
                   "the whole cheat is ignored",
                   index, text, malformed, count);
    } else if (unknown + outside > 0) {
        brimir_log(RETRO_LOG_WARN,
                   "Cheat %u: '%s': %zu sub-codes, ignored: %u master, %u unknown type, %u outside work RAM",
                   index, text, count, master, unknown, outside);
    } else {
        brimir_log(RETRO_LOG_INFO, "Cheat %u %s: '%s' (%zu sub-codes, %u master codes ignored)", index,
                   enabled ? "on" : "off", text, count, master);
    }
}

// Deterministic runs (brimir_vkhost --fixed-date): BRIMIR_RTC_FIXED pins the RTC to the virtual clock at a fixed date
// ("" or "1": brimir_bench's lockstep date; "YYYY-MM-DD[THH:MM:SS]"). Called after the content is loaded, which loads
// the persisted RTC file, and before the first frame. No user-facing option.
static void apply_fixed_rtc(void) {
    const char* env = std::getenv("BRIMIR_RTC_FIXED");
    if (env == nullptr || !g_core) {
        return;
    }
    util::datetime::DateTime date{};
    if (!brimir::ParseFixedRtcDate(env, date)) {
        brimir_log(RETRO_LOG_ERROR, "BRIMIR_RTC_FIXED=%s is not 1 or YYYY-MM-DD[THH:MM:SS]; RTC not fixed", env);
        return;
    }
    util::datetime::DateTime guest{};
    const bool fixed = brimir::ApplyFixedRtc(*g_core, date, &guest);
    const auto text = [](const util::datetime::DateTime& t) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u %02u:%02u:%02u", static_cast<int>(t.year),
                      unsigned{t.month}, unsigned{t.day}, unsigned{t.hour}, unsigned{t.minute}, unsigned{t.second});
        return std::string(buffer);
    };
    if (!fixed) {
        brimir_log(RETRO_LOG_ERROR,
                   "BRIMIR_RTC_FIXED=%s: the RTC cannot be fixed to %s (the guest would read %s: not representable in "
                   "this host's time zone, or the core is not initialized); RTC not fixed",
                   env, text(date).c_str(), text(guest).c_str());
        return;
    }
    brimir_log(RETRO_LOG_INFO, "RTC fixed to %s (virtual clock, guest time %s)", text(date).c_str(),
               text(guest).c_str());
}

static const char* brimir_region_name(brimir::DiscRegion region) {
    switch (region) {
    case brimir::DiscRegion::Japan: return "Japan";
    case brimir::DiscRegion::UsEu: return "US/EU";
    default: return "unknown";
    }
}

// A disc header's area code symbols from the ymir::media::AreaCode bitmap ("none" for 0).
static std::string brimir_area_letters(uint16_t area_codes) {
    static constexpr struct { unsigned bit; char letter; } kAreas[] = {
        {0x1, 'J'}, {0x2, 'T'}, {0x4, 'U'}, {0x5, 'B'}, {0x6, 'K'}, {0xA, 'A'}, {0xC, 'E'}, {0xD, 'L'},
    };
    std::string letters;
    for (const auto& area : kAreas) {
        if (area_codes & (1u << area.bit)) {
            letters += area.letter;
        }
    }
    return letters.empty() ? "none" : letters;
}

RETRO_API bool retro_load_game(const struct retro_game_info* game) {
    // No content (SET_SUPPORT_NO_GAME): the BIOS runs on its own, like brimir_bench without --game.
    const bool no_content = game == nullptr;

    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);

    if (!g_core) {
        brimir_log(RETRO_LOG_ERROR, "Core not initialized");
        return false;
    }

    // New content may report the same resolution as the previous one; force a
    // geometry update on its first frame regardless.
    reset_frame_state();

    brimir_log(RETRO_LOG_INFO, "Loading game: %s", no_content ? "(none)" : game->path ? game->path : "unknown");

    // Set pixel format - XRGB8888 for full quality (lossless from VDP output)
    // This is a lossless channel swap from VDP's native XBGR8888, replacing the old
    // lossy RGB565 conversion.
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        brimir_log(RETRO_LOG_ERROR, "XRGB8888 pixel format is not supported by this frontend");
        return false;
    }
    
    // Get system directory from RetroArch (needed for BIOS and RTC)
    const char* system_dir = nullptr;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) || !system_dir) {
        brimir_log(RETRO_LOG_ERROR, "Could not get system directory");
        return false;
    }
    
    // Get save directory from RetroArch for persistent backup RAM
    const char* save_dir = nullptr;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) || !save_dir) {
        brimir_log(RETRO_LOG_WARN, "Could not get save directory, using temp directory");
    } else {
        brimir_log(RETRO_LOG_INFO, "Save directory: %s", save_dir);
    }
    
    // Get BIOS preference from core options
    const char* bios_pref = get_option_value("brimir_bios", "auto");

    // Load the BIOS: Auto chooses for each content (its region), an explicit choice once
    if (brimir::ShouldSelectBios(bios_pref, g_core->IsIPLLoaded())) {
        brimir_log(RETRO_LOG_INFO, "Loading BIOS...");
        
        // Map of BIOS option keys to filenames
        struct BIOSEntry {
            const char* key;
            const char* filename;
        };
        
        static const BIOSEntry bios_map[] = {
            { "jp_101", "Sega Saturn BIOS v1.01 (JAP).bin" },
            { "jp_100", "Sega Saturn BIOS v1.00 (JAP).bin" },
            { "us_101", "sega_101.bin" },
            { "us_100", "mpr-17933.bin" },
            { "eu_100", "sega_100.bin" },
            { "eu_alt", "Sega Saturn BIOS (EUR).bin" },
        };
        static const int bios_count = sizeof(bios_map) / sizeof(bios_map[0]);
        
        bool bios_loaded = false;
        
        // If user selected a specific BIOS, try only that one
        if (strcmp(bios_pref, "auto") != 0) {
            for (int i = 0; i < bios_count; i++) {
                if (strcmp(bios_pref, bios_map[i].key) == 0) {
                    char bios_path[4096];
                    snprintf(bios_path, sizeof(bios_path), "%s%c%s", system_dir,
#ifdef _WIN32
                        '\\',
#else
                        '/',
#endif
                        bios_map[i].filename);
                    
                    if (g_core->LoadIPLFromFile(bios_path)) {
                        brimir_log(RETRO_LOG_INFO, "Loaded BIOS: %s", bios_map[i].filename);
                        bios_loaded = true;
                    } else {
                        brimir_log(RETRO_LOG_ERROR, "Selected BIOS not found: %s", bios_map[i].filename);
                    }
                    break;
                }
            }
        } else {
            // Auto: the BIOS of the disc's region, else any available one; no content: Japanese first (bios_select.hpp)
            const uint16_t area_codes = g_core->ReadContentAreaCodes(no_content ? nullptr : game->path);
            const brimir::DiscRegion region = brimir::DiscRegionFromAreaCodes(area_codes);
            const std::string filename = g_core->LoadAutoIPL(system_dir, region);
            if (!filename.empty()) {
                const brimir::DiscRegion loaded = g_core->GetLoadedIPLRegion();
                brimir_log(RETRO_LOG_INFO,
                           "Auto-detected and loaded BIOS: %s (disc areas: %s, disc region: %s, BIOS region: %s)",
                           filename.c_str(), brimir_area_letters(area_codes).c_str(), brimir_region_name(region),
                           brimir_region_name(loaded));
                bios_loaded = true;
            }
        }
        
        if (!bios_loaded) {
            brimir_log(RETRO_LOG_ERROR, "Failed to load BIOS. Place one of the following in system directory: %s", system_dir);
            brimir_log(RETRO_LOG_ERROR, "  - Sega Saturn BIOS v1.01 (JAP).bin (JP v1.01)");
            brimir_log(RETRO_LOG_ERROR, "  - Sega Saturn BIOS v1.00 (JAP).bin (JP v1.00)");
            brimir_log(RETRO_LOG_ERROR, "  - sega_101.bin (JP v1.01)");
            brimir_log(RETRO_LOG_ERROR, "  - mpr-17933.bin (US/EU v1.00)");
            brimir_log(RETRO_LOG_ERROR, "  - sega_100.bin (JP v1.00)");
            brimir_log(RETRO_LOG_ERROR, "  - Sega Saturn BIOS (EUR).bin (US/EU)");
            brimir_log(RETRO_LOG_ERROR, "Note: sega1003.bin (JP v1.003) is NOT supported due to compatibility issues");
            return false;
        }
    }
    
    // Before LoadGame, which applies the mode again once the RTC file is read (no content: applies now).
    g_options.rtc_mode = get_option_value("brimir_rtc_mode", "virtual");
    g_core->SetRTCMode(g_options.rtc_mode == "host" ? brimir::RTCMode::Host : brimir::RTCMode::Virtual);
    g_options.cartridge = get_option_value("brimir_cartridge", "auto");
    g_core->SetCartridgeOverride(brimir_cartridge_choice(g_options.cartridge.c_str()));
    g_options.sh2_cache = get_option_value("brimir_sh2_cache", "disabled");
    g_core->SetSH2CacheEmulation(g_options.sh2_cache == "enabled");
    g_options.cdblock_lle = get_option_value("brimir_cdblock_lle", "disabled");
    g_core->SetCDBlockLLE(g_options.cdblock_lle == "enabled");
    if (no_content) {
        g_core->ApplyLoadTimeOptions(system_dir); // LoadGame applies them for content
    }

    if (!no_content) {
        brimir_log(RETRO_LOG_INFO, "Loading game: %s", game->path);

        if (!g_core->LoadGame(game->path, save_dir, system_dir)) {
            const std::string& error = g_core->GetLastError();
            brimir_log(RETRO_LOG_ERROR, "Failed to load game: %s",
                error.empty() ? "(no error message)" : error.c_str());
            return false;
        }

        brimir_log(RETRO_LOG_INFO, "Game loaded successfully");
        brimir_log(RETRO_LOG_INFO, "Cartridge: %s", g_core->GetCartridgeDescription().c_str());
        if (const std::string& cartMessage = g_core->GetLastCartridgeMessage(); !cartMessage.empty()) {
            brimir_log(RETRO_LOG_WARN, "Cartridge: %s", cartMessage.c_str());
            if (environ_cb) {
                static std::string text;
                text = "Brimir: " + cartMessage;
                retro_message message{text.c_str(), 600};
                environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
            }
        }
    }

    // An unrecognised ROM is used (Ymir's database lists the known dumps), with a warning
    const std::string& cdblockDescription = g_core->GetCDBlockDescription();
    const bool unrecognisedROM = g_core->IsCDBlockLLEActive() && !g_core->IsCDBlockROMRecognised();
    brimir_log(unrecognisedROM ? RETRO_LOG_WARN : RETRO_LOG_INFO, "CD Block: %s", cdblockDescription.c_str());
    if (unrecognisedROM && environ_cb) {
        static std::string text;
        text = "Brimir: CD Block LLE uses " + cdblockDescription + "; it may not work";
        retro_message message{text.c_str(), 600};
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
    }
    if (const std::string& cdblockMessage = g_core->GetLastCDBlockMessage(); !cdblockMessage.empty()) {
        brimir_log(RETRO_LOG_WARN, "CD Block: %s", cdblockMessage.c_str());
        if (environ_cb) {
            static std::string text;
            text = "Brimir: " + cdblockMessage;
            retro_message message{text.c_str(), 600};
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
        }
    }
    if (g_core->IsSH2CacheEmulationActive()) {
        brimir_log(RETRO_LOG_INFO, "SH-2 cache emulation: on (%s)",
                   g_options.sh2_cache == "enabled" ? "option" : "game database");
    }
    g_jitCacheNoted = false;

    apply_fixed_rtc();

    // Apply core options after the disc is loaded. Region autodetection happens
    // during disc loading, so options that disable it must not be applied until
    // afterwards (otherwise the system would stay at the default Japan area code).
#ifdef BRIMIR_VULKAN
    g_loadingContent = true;
#endif
    apply_core_options(true);

#ifdef BRIMIR_VULKAN
    g_loadingContent = false;
    // The renderer applies at content load. With Vulkan the frontend builds the context after this function
    // returns (context_reset); the maximum geometry reported below already accounts for it.
    g_vulkanRequested = false;
    g_options.renderer = get_option_value("brimir_renderer", "software");
    if (g_options.renderer == "vulkan") {
        using brimir::libretro::VulkanRequest;
        std::string reason;
        switch (brimir::libretro::RequestVulkanContext(environ_cb, vulkan_context_reset, vulkan_context_destroy,
                                                       log_cb, reason)) {
        case VulkanRequest::Accepted:
            g_vulkanRequested = true;
            brimir_log(RETRO_LOG_INFO, "Vulkan renderer requested");
            break;
        case VulkanRequest::NotVulkanDriver:
            brimir_log(RETRO_LOG_WARN,
                       "Vulkan renderer selected but the frontend video driver is not Vulkan; using software");
            break;
        case VulkanRequest::Refused:
            brimir_log(RETRO_LOG_WARN, "Vulkan renderer unavailable, falling back to software: %s", reason.c_str());
            break;
        }
    }
    note_internal_resolution(); // the software renderer: native frames
#endif

    update_system_av_info_for_region();

    g_core->SetRenderer("software");

    // Work RAM view for achievements and RetroArch cheats / memory viewer.
    publish_ram_view();
    g_memdesc[0].ptr = g_ramView.Data();
    g_memdesc[1].ptr = g_ramView.Data() + brimir::RamView::kHighOffset;

    static struct retro_memory_map mmap = {
        .descriptors     = g_memdesc,
        .num_descriptors = g_memdesc_count,
    };
    environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap);

    bool achievements = true;
    environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);

    return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
    // Not used for Saturn
    (void)game_type;
    (void)info;
    (void)num_info;
    return false;
}

RETRO_API void retro_unload_game(void) {
    brimir_log(RETRO_LOG_INFO, "Unloading game");

    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    // A C++ exception must never cross the libretro C ABI: the frontend cannot catch it and
    // the process dies (issue #7). Log it instead so the cause is visible in the frontend log.
    try {
        if (g_core) {
            g_core->UnloadGame();
            log_cartridge_save_error();
            g_cartridgeFlushFrames = 0;
            g_stateLoadedBeforeRun = false;
#ifdef BRIMIR_VULKAN
            // The next content chooses its renderer again. The frontend has normally destroyed the context already
            // (the renderer is detached then). The state is discarded: no VDP1 readback.
            std::string error;
            if (g_vulkanOutput && !g_vulkanOutput->DetachRenderer(error, true)) {
                brimir_log(RETRO_LOG_ERROR, "Vulkan VDP2 renderer detach: %s", error.c_str());
            }
            g_core->UseSoftwareRenderer();
#endif
        }
    } catch (const std::exception& e) {
        brimir_log(RETRO_LOG_ERROR, "Exception while unloading game: %s", e.what());
    } catch (...) {
        brimir_log(RETRO_LOG_ERROR, "Unknown exception while unloading game");
    }
    reset_frame_state();
    g_ramView.Clear();
    g_cheats.Reset();
#ifdef BRIMIR_VULKAN
    // The frontend destroys the context (context_destroy) before unloading; nothing GPU-side happens here.
    g_vulkanRequested = false;
#endif
}

// Disk control callbacks

static bool disk_set_eject_state(bool ejected) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) return false;
    return g_core->SetEjectState(ejected);
}

static bool disk_get_eject_state(void) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) return false;
    return g_core->GetEjectState();
}

static unsigned disk_get_image_index(void) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) return 0;
    return static_cast<unsigned>(g_core->GetCurrentDiscIndex());
}

static bool disk_set_image_index(unsigned index) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) return false;
    return g_core->SetDiscIndex(index);
}

static unsigned disk_get_num_images(void) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) return 0;
    return static_cast<unsigned>(g_core->GetNumDiscs());
}

static bool disk_replace_image_index(unsigned index, const struct retro_game_info* info) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) return false;
    if (info) {
        return g_core->ReplaceDiscIndex(index, info->path);
    } else {
        return g_core->ReplaceDiscIndex(index, nullptr);
    }
}

static bool disk_add_image_index(void) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) return false;
    return g_core->AddDiscIndex();
}

static bool disk_set_initial_image(unsigned index, const char* path) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) return false;
    return g_core->SetInitialDisc(index, path);
}

static bool disk_get_image_path(unsigned index, char* s, size_t len) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) return false;
    return g_core->GetDiscPath(index, s, len);
}

static bool disk_get_image_label(unsigned index, char* s, size_t len) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) return false;
    return g_core->GetDiscLabel(index, s, len);
}

RETRO_API unsigned retro_get_region(void) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (g_core && g_core->IsGameLoaded()) {
        return g_core->GetConsoleRegion() == brimir::ConsoleRegion::PAL
               ? RETRO_REGION_PAL
               : RETRO_REGION_NTSC;
    }
    return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned id) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) {
        return nullptr;
    }
    
    switch (id) {
        case RETRO_MEMORY_SAVE_RAM:
            return g_core->GetSRAMData();
        case RETRO_MEMORY_SYSTEM_RAM:
            return g_ramView.Data();
        default:
            return nullptr;
    }
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
    std::lock_guard<std::recursive_mutex> lock(g_coreMutex);
    if (!g_core) {
        return 0;
    }
    
    switch (id) {
        case RETRO_MEMORY_SAVE_RAM:
            return g_core->GetSRAMSize();
        case RETRO_MEMORY_SYSTEM_RAM:
            return brimir::RamView::kSize;
        default:
            return 0;
    }
}
