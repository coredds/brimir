// Brimir - Core wrapper for Ymir emulator
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "audio_ring_buffer.hpp"
#include "bios_select.hpp"
#include "peripheral_input.hpp"
#include "profiler.hpp"

// Include Saturn headers for test access
// Note: Only include in test builds to keep libretro API clean
#ifdef BRIMIR_BUILD_TESTS
#include <ymir/sys/saturn.hpp>
#endif

// Forward declarations to avoid including Ymir headers in libretro builds
namespace ymir {
struct Saturn;

namespace smpc {
struct PersistentSMPCData;
}

namespace savestate {
struct SaveState;
}

namespace cart {
class BackupMemoryCartridge;
}

namespace peripheral {
class ControlPad;
struct PeripheralReport;
enum class Button : uint16_t;
}
}

namespace brimir::gpu {
class VulkanVDPRenderer;
} // namespace brimir::gpu

namespace brimir::jit {
class CompileWorker;
class Executor;
enum class BackendKind : uint8_t;
} // namespace brimir::jit

namespace brimir {

enum class ConsoleRegion {
    NTSC,
    PAL,
    Unknown,
};

/// @brief How the emulated RTC keeps time (option brimir_rtc_mode)
enum class RTCMode {
    Virtual, ///< advances with emulated time only (deterministic; held in save states)
    Host,    ///< reads the host clock plus the user's offset
};

/// @brief The cartridge LoadGame inserts (option brimir_cartridge)
enum class CartridgeChoice {
    Auto,        ///< the game database's cartridge (DRAM, ROM or backup memory), none for other games
    None,        ///< no cartridge
    DRAM8Mbit,   ///< 1 MB RAM cartridge
    DRAM32Mbit,  ///< 4 MB RAM cartridge
    Backup,      ///< 4 Mbit (512 KiB) backup memory cartridge, saved to <game>.bcr
    ROM_KOF95,   ///< The King of Fighters '95 ROM cartridge (mpr-18811-mx.ic1 in the system directory)
    ROM_Ultraman ///< Ultraman: Hikari no Kyojin Densetsu ROM cartridge (mpr-19367-mx.ic1 in the system directory)
};

/// @brief The output transforms the GPU frame output applies to a raw frame (see SetOverscanCrop/SetRotation)
struct FrameOutputSettings {
    uint32_t cropH = 0;    ///< total horizontal overscan crop
    uint32_t cropV = 0;    ///< total vertical overscan crop
    uint32_t rotation = 0; ///< 0, 90, 180 or 270
};

/// @brief Wraps the Ymir Saturn emulator for use with libretro
class CoreWrapper {
public:
    /// @brief Version word in the BRI2 save-state header. Version 4 serializes the state field by field
    /// (savestate_codec.hpp); earlier versions held the raw struct, including stale heap pointers, and are
    /// rejected, as are the older unversioned "BRIM" states. Version 5 keeps the same layout but stores the
    /// VDP2 fetchers' palette numbers whole; version 4 states are converted on load. Version 6 appends the state
    /// upstream save states omit (ymir::savestate::BrimirSaveState); version 4 and 5 states load without it. Version 7
    /// adds the SCSP slot modulation inputs to it; version 6 states load without them. Version 8 keeps the layout:
    /// the SCU cartridge image holds a backup memory cartridge's data bytes and is empty for a ROM cartridge (the
    /// ROM is the dump inserted at content load); version 7 states have no image for either. Version 9 adds the
    /// controller port peripherals' state and the bridge's input state per port (BrimirSaveState::Peripherals);
    /// version 7 and 8 states load without them. Version 10 adds the Virtua Gun jitter's random state
    /// (BrimirSaveState::VirtuaGun); version 9 states load without it. Version 11 records the saving session's SH-2
    /// cache and SCSP step granularity (BrimirSaveState::Settings, diagnostic: GetLastStateWarning); version 10
    /// states load without them.
    static constexpr uint32_t kSaveStateVersion = 11;

    /// @brief Oldest BRI2 version LoadState accepts
    static constexpr uint32_t kOldestLoadableSaveStateVersion = 4;

    CoreWrapper();
    ~CoreWrapper();

    // Prevent copying
    CoreWrapper(const CoreWrapper&) = delete;
    CoreWrapper& operator=(const CoreWrapper&) = delete;

    /// @brief Initialize the emulator
    /// @return true if successful
    bool Initialize();

    /// @brief Shutdown the emulator
    void Shutdown();

    /// @brief Load a game from the given path
    /// @param path Path to the game file
    /// @param save_directory Directory where save files (.srm, backup RAM) are stored
    /// @param system_directory Directory where system-wide files (RTC, etc.) are stored
    /// @return true if successful
    bool LoadGame(const char* path, const char* save_directory = nullptr, const char* system_directory = nullptr);

    /// @brief Get the last error message from game loading
    /// @return Last error message, or empty string if none
    const std::string& GetLastError() const { return m_lastError; }

    /// @brief Unload the current game
    void UnloadGame();

    /// @brief Get pointer to the canonical backup RAM (SRAM) buffer used by RetroArch.
    /// @return Pointer to SRAM data, or nullptr if not available
    void* GetSRAMData();

    /// @brief Get size of backup RAM (SRAM)
    /// @return Size in bytes
    size_t GetSRAMSize() const;

    /// @brief Replace the SRAM buffer with external data (e.g. RetroArch's .srm)
    /// @param data New SRAM data, must be exactly GetSRAMSize() bytes
    /// @param size Number of bytes in data
    /// @return true if the data was accepted (size matches and data non-null)
    bool SetSRAMData(const uint8_t* data, size_t size);

    /// @brief Get pointer to Work RAM Low, in Saturn (big-endian) byte order. The libretro layer gives the frontend
    /// the Beetle-compatible view of Low and High instead (brimir/ram_view.hpp).
    /// @return Pointer to WRAM Low data (1MiB), or nullptr if not available
    void* GetSystemRAMData();

    /// @brief Get size of system RAM
    /// @return Size in bytes (1MiB)
    size_t GetSystemRAMSize() const;

    /// @brief Get raw pointer to backup RAM buffer without triggering .srm sync side effects.
    /// Used by memory descriptors; callers needing full SRAM lifecycle MUST use GetSRAMData().
    /// @return Pointer to SRAM buffer, or nullptr if not available
    void* GetSRAMRawPointer();

    /// @brief Get raw pointer to system RAM without any side effects.
    /// @return Pointer to WRAM Low data, or nullptr if not available
    void* GetSystemRAMRawPointer();

    /// @brief Get raw pointer to WRAM High without any side effects.
    /// @return Pointer to WRAM High data (1MiB), or nullptr if not available
    void* GetSystemRAMHighRawPointer();

    /// @brief Get size of WRAM High
    /// @return Size in bytes (1MiB)
    size_t GetSystemRAMHighSize() const;

    /// @brief Force refresh SRAM from Ymir's backup RAM
    /// This reads Ymir's internal backup RAM into our buffer
    void RefreshSRAMFromEmulator();

    /// @brief Run one frame of emulation
    void RunFrame();

    /// @brief Reset the emulator
    void Reset();

    /// @brief Load IPL (BIOS) ROM from memory
    /// @param data BIOS data (should be 512KB)
    /// @return true if BIOS was loaded successfully
    bool LoadIPL(std::span<const uint8_t> data);

    /// @brief Load IPL (BIOS) ROM from file
    /// @param path Path to BIOS file
    /// @return true if BIOS was loaded successfully
    bool LoadIPLFromFile(const char* path);

    /// @brief Check if BIOS/IPL is loaded
    /// @return true if BIOS is loaded
    bool IsIPLLoaded() const { return m_iplLoaded; }

    /// @brief The region of the loaded BIOS per Ymir's IPL ROM database (Unknown: none loaded, not listed, Korean)
    DiscRegion GetLoadedIPLRegion() const;

    /// @brief The compatible area codes (ymir::media::AreaCode bitmap) of the disc LoadGame would load from `path`
    /// (for an M3U, its first disc or the frontend's initial image), read without loading it; 0 when unreadable.
    uint16_t ReadContentAreaCodes(const char* path) const;

    /// @brief brimir_bios = auto: loads the BIOS of `region` (the content's, DiscRegionFromAreaCodes; no content:
    /// Unknown) from `system_dir`, else any available one (bios_select.hpp). Returns the chosen file name, empty when
    /// none loads.
    std::string LoadAutoIPL(const char* system_dir, DiscRegion region);

    /// @brief Get the currently configured console region for libretro reporting
    ConsoleRegion GetConsoleRegion() const;

    /// @brief Set controller input state for a port: SetPeripheralInput with only the RetroPad buttons
    /// @param port Port number (0 or 1)
    /// @param buttons Button states (libretro button mask)
    void SetControllerState(unsigned int port, uint16_t buttons);

    /// @brief Connects a peripheral to a controller port (0 or 1); nothing changes if it is connected already. The new
    /// peripheral starts in its power-on state with released controls. Call between frames; before Initialize the
    /// choice is kept and applied there. Both ports start with the Control Pad.
    void SetPeripheral(unsigned port, PeripheralKind kind);
    PeripheralKind GetPeripheral(unsigned port) const;

    /// @brief This frame's libretro input for a port (call before RunFrame): the port's edges advance
    /// (AdvancePortInput) and the report its peripheral reads during the frame is mapped (peripheral_input.hpp).
    void SetPeripheralInput(unsigned port, const InputSnapshot& input);

    /// @brief The port's input state across frames (3D Control Pad mode, Mission Stick throttle latch); in states
    const PortInputState& GetPortInputState(unsigned port) const { return m_portInput[port & 1]; }

    /// @brief The screen the Virtua Gun aims at: the current Saturn resolution and the overscan crop and rotation
    GunScreen GetGunScreen() const;

    /// @brief Get the current video frame buffer
    /// @return Pointer to framebuffer data, or nullptr if not available
    const void* GetFramebuffer() const;

    /// @brief Get the current framebuffer width
    /// @return Width in pixels
    unsigned int GetFramebufferWidth() const;

    /// @brief Get the current framebuffer height
    /// @return Height in pixels
    unsigned int GetFramebufferHeight() const;

    /// @brief Get the current framebuffer pitch (bytes per scanline)
    /// @return Pitch in bytes
    unsigned int GetFramebufferPitch() const;

    /// @brief Hand frames to a GPU frame output instead of the CPU path. While active, each completed frame is
    /// kept as Ymir's native XBGR8888 frame (RawFrame) with the crop/rotation settings current at completion
    /// (RawFrameSettings), and the CPU convert/crop/rotate is skipped: GetFramebuffer() and its size keep the last
    /// software frame. Changing the state drops the kept frame.
    void SetGpuOutputActive(bool active);
    bool IsGpuOutputActive() const { return m_gpuOutputActive; }

    /// @brief The last RunFrame completed a frame on the CPU path (GetFramebuffer() holds it), as opposed to keeping
    /// an older one (no frame ended, or the frame went to the GPU output or the Vulkan VDP renderer)
    bool CpuFrameFromLastRun() const { return m_cpuFrameFromLastRun; }

    /// @brief The last frame completed while the GPU output was active (tightly packed XBGR8888), or nullptr
    const uint32_t* RawFrame() const { return m_rawWidth != 0 ? m_rawFrame.data() : nullptr; }
    uint32_t RawFrameWidth() const { return m_rawWidth; }
    uint32_t RawFrameHeight() const { return m_rawHeight; }
    const FrameOutputSettings& RawFrameSettings() const { return m_rawSettings; }

    /// @brief Installs the Vulkan VDP renderer (VDP2 on the GPU, VDP1 in software; BRIMIR_VULKAN builds) in place of
    /// the software renderer, carrying the renderer save-state section over, with VDP1 threading as configured. Its
    /// frames stay on the device (LastDeviceFrame); the caller attaches it to a device. Call between frames. Returns
    /// the renderer (also when it was installed already), or nullptr with the software renderer kept.
    gpu::VulkanVDPRenderer* InstallVulkanRenderer();

    /// @brief Switches back from the Vulkan VDP renderer to the software renderer, carrying the renderer save-state
    /// section over; the software renderer gets the configured threading. The caller has detached the Vulkan
    /// renderer from its device. No-op when it is not installed. Call between frames.
    void UseSoftwareRenderer();

    /// @brief The installed Vulkan VDP renderer, or nullptr
    gpu::VulkanVDPRenderer* GetVulkanRenderer() const { return m_vulkanRenderer; }

    /// @brief A frame the Vulkan VDP renderer completed on the device: its size and the crop/rotation settings
    /// current at completion (no CPU copy). Width 0 until the first frame after the install.
    struct DeviceFrame {
        uint32_t width = 0;
        uint32_t height = 0;
        FrameOutputSettings settings;
    };
    const DeviceFrame& LastDeviceFrame() const { return m_deviceFrame; }

    /// @brief Get audio samples for this frame
    /// @param buffer Buffer to write samples to
    /// @param max_samples Maximum number of stereo samples to write
    /// @return Number of stereo samples written
    size_t GetAudioSamples(int16_t* buffer, size_t max_samples);

    /// @brief Get the size needed for a save state
    /// @return Size in bytes; stable while a game is loaded, larger for games with a RAM cartridge
    size_t GetStateSize() const;

    /// @brief Save state to buffer
    /// @param data Buffer to write state to (must be at least GetStateSize() bytes)
    /// @param size Size of the buffer
    /// @return true if successful
    bool SaveState(void* data, size_t size);

    /// @brief Load state from buffer
    /// @param data Buffer containing state data
    /// @param size Size of the buffer
    /// @return true if successful
    bool LoadState(const void* data, size_t size);

    /// @brief Why the last LoadState() rejected a well-formed state, or an empty string
    const std::string& GetLastStateError() const { return m_lastStateError; }

    /// @brief How the state the last LoadState() loaded was saved differently from this session (SH-2 cache
    /// emulation, SCSP step granularity: not emulation state, so the session keeps its own and may run differently
    /// from the saving one), or an empty string
    const std::string& GetLastStateWarning() const { return m_lastStateWarning; }

    /// @brief Check if the emulator is initialized
    /// @return true if initialized
    bool IsInitialized() const { return m_initialized; }

    /// @brief Check if a game is currently loaded
    /// @return true if a game is loaded
    bool IsGameLoaded() const { return m_gameLoaded; }

    /// @brief Get the Ymir Saturn instance (for advanced access)
    /// @return Pointer to Saturn instance, or nullptr if not initialized
    ymir::Saturn* GetSaturn() { return m_saturn.get(); }

    /// @brief Set audio interpolation mode
    /// @param mode Interpolation mode: "linear" or "nearest"
    void SetAudioInterpolation(const char* mode);

    /// @brief Set CD read speed multiplier
    /// @param speed Speed multiplier (2-200)
    void SetCDReadSpeed(uint8_t speed);

    /// @brief Set whether disc images are preloaded entirely into RAM
    /// @param enable True to load the whole image into memory (avoids streaming hitches)
    void SetDiscPreloadEnabled(bool enable) { m_discPreload = enable; }

    /// @brief Set SH-2 CPU overclock factor
    /// @param factor Overclock percentage (100-300), 100 = stock
    void SetSH2OverclockFactor(uint32_t factor);

    /// @brief Set autodetect region from disc
    /// @param enable True to enable autodetect
    void SetAutodetectRegion(bool enable);

    /// @brief Set the RTC mode. Applies now to the running system and again at each LoadGame, after the RTC file is
    /// read. Virtual: the clock advances with emulated time, survives hard resets, and starts at the host's local
    /// time (plus the file's offset to the host clock) when no virtual time is stored yet. Host: reads the host
    /// clock plus the stored offset.
    void SetRTCMode(RTCMode mode);
    RTCMode GetRTCMode() const { return m_rtcMode; }

    /// @brief ROM cartridge dump file names (in the system directory), as Beetle and Kronos name them
    static constexpr const char* kKOF95ROMFileName = "mpr-18811-mx.ic1";
    static constexpr const char* kUltramanROMFileName = "mpr-19367-mx.ic1";
    /// @brief Size of the backup memory cartridge and of its <game>.bcr file (4 Mbit of data bytes)
    static constexpr size_t kBackupCartSize = 512 * 1024;

    /// @brief Choose the cartridge. Applies at the next LoadGame (the cartridge is inserted before the BIOS boots).
    void SetCartridgeOverride(CartridgeChoice choice) { m_cartridgeChoice = choice; }
    CartridgeChoice GetCartridgeOverride() const { return m_cartridgeChoice; }

    /// @brief Why the last LoadGame started without the chosen cartridge or with an unsaved one (missing or wrong ROM
    /// dump, unreadable .bcr file), or an empty string. For the frontend's on-screen message and log.
    const std::string& GetLastCartridgeMessage() const { return m_cartridgeMessage; }

    /// @brief The cartridge the last LoadGame inserted, for the log (e.g. "4 MB RAM"), or "none"
    const std::string& GetCartridgeDescription() const { return m_cartridgeDescription; }

    /// @brief Writes the backup cartridge's save file (<game>.bcr) if its contents changed since the last write or
    /// the load; a RAM cartridge's <game>.cart is written at unload only (volatile on hardware, 4-6 MiB). Reads the cartridge only (emulation state is untouched), so it may run between any
    /// two frames. UnloadGame writes it too. A failure is kept for TakeCartridgeSaveError; a flush does not wait to
    /// retry a refused rename, and after a failure it does nothing until the contents change again (UnloadGame still
    /// tries, with one retry).
    /// @return false if the file could not be written
    bool FlushCartridgeSave();

    /// @brief Takes why the last cartridge save failed (the file and where the data is), or an empty string
    std::string TakeCartridgeSaveError() { return std::exchange(m_cartridgeSaveError, std::string{}); }

    /// @brief Writes data into a backup memory cartridge (data byte i at bus address 2i + 1), only the bytes that
    /// differ; data must be the cartridge's size. Used by LoadState (public for the cost measurement test).
    static void RestoreBackupCartData(ymir::cart::BackupMemoryCartridge& cart, std::span<const uint8_t> data);

    /// @brief CD Block ROM file names searched in the system directory, in this order (MAME names the v1.05 dump
    /// cdb105.bin). The first one of 64 KiB is used.
    static constexpr const char* kCDBlockROMFileNames[] = {"cdb106.bin", "cdb105.bin", "cdb104.bin"};

    /// @brief SH-2 cache emulation (option brimir_sh2_cache). Applies at the next LoadGame or ApplyLoadTimeOptions;
    /// games the Ymir database marks keep their forced cache emulation either way.
    void SetSH2CacheEmulation(bool enable) { m_sh2CacheRequested = enable; }
    /// @brief Whether SH-2 cache emulation is active (the option or the game database); the SH-2 JIT is not used then
    bool IsSH2CacheEmulationActive() const;

    /// @brief Low-level CD Block emulation (option brimir_cdblock_lle). Applies at the next LoadGame or
    /// ApplyLoadTimeOptions, with the CD Block ROM from the system directory (kCDBlockROMFileNames); without one the
    /// CD Block stays high-level and GetLastCDBlockMessage says why.
    void SetCDBlockLLE(bool enable) { m_cdblockLLERequested = enable; }
    /// @brief Whether the CD Block is emulated low-level now (a save state of the other mode switches it)
    bool IsCDBlockLLEActive() const { return m_cdblockLLEActive; }
    /// @brief Why the last ApplyLoadTimeOptions kept the CD Block high-level although low-level was chosen (no or a
    /// wrong-size ROM), or an empty string. For the frontend's on-screen message and log.
    const std::string& GetLastCDBlockMessage() const { return m_cdblockMessage; }
    /// @brief The CD Block emulation the last ApplyLoadTimeOptions chose, for the log (e.g. "high-level")
    const std::string& GetCDBlockDescription() const { return m_cdblockDescription; }
    /// @brief Whether low-level emulation runs a CD Block ROM Ymir's database lists (false without LLE, or for an
    /// unrecognised dump, which is used anyway)
    bool IsCDBlockROMRecognised() const;

    /// @brief SCSP step granularity, 0 (a sample, 32 slots, at a time: Ymir's default) to 5 (one slot at a time).
    /// Applies now (Ymir switches between steps live). Not emulation state: states record it for a warning only.
    void SetSCSPStepGranularity(uint32_t granularity);
    uint32_t GetSCSPStepGranularity() const { return m_scspStepGranularity; }

    /// @brief Applies the load-time accuracy options (SH-2 cache, CD Block LLE with its ROM from systemDirectory).
    /// LoadGame calls it; a BIOS-only boot (no content) calls it before the first frame. Switching the CD Block mode
    /// hard-resets the system.
    void ApplyLoadTimeOptions(const char* systemDirectory);
    
    /// @brief Set renderer type (Ymir hw layer only supports software)
    /// @param renderer Renderer string: "software"
    void SetRenderer(const char* renderer);
    const char* GetActiveRenderer() const;

    /// @brief Set threaded VDP1 rendering
    void SetThreadedVDP1(bool enable);

    /// @brief Set threaded VDP2 rendering
    void SetThreadedVDP2(bool enable);

    /// @brief Run the SH-2 CPUs through the experimental JIT (see design/sh2-jit.md).
    /// Remembered across Initialize(). Has no effect while SH-2 cache emulation is active.
    void SetSH2JitEnabled(bool enable);
    bool IsSH2JitEnabled() const { return m_sh2JitEnabled; }

    /// @brief Select the JIT code backend (default: jit::DefaultBackend()). An unavailable kind
    /// selects the IR backend. Changing it drops both executors (and their compiled blocks); if the
    /// JIT is enabled they are recreated with the new backend. Remembered across Initialize().
    void SetSH2JitBackend(jit::BackendKind kind);
    jit::BackendKind GetSH2JitBackend() const { return m_sh2JitBackend; }

    /// @brief Allow compiling native code on one extra thread (default true, the "auto" option).
    /// The worker exists only if this is true, jit::CompileThreadSupported() holds and the backend
    /// is x64; otherwise compiles run on the emulation thread. Changing it drops both executors
    /// (and their compiled blocks) and recreates them if the JIT is enabled. Remembered across
    /// Initialize().
    void SetSH2JitCompileThread(bool enable);
    bool IsSH2JitCompileThreadEnabled() const { return m_sh2JitCompileThread; }

    /// @brief The compile worker shared by both executors, or nullptr if there is none.
    jit::CompileWorker* GetSH2JitCompileWorker() const { return m_jitWorker.get(); }

    /// @brief Takes the warning left when the compile worker's thread could not start (the JIT then
    /// compiles on the emulation thread), or an empty string if there is none. Each failure is
    /// reported once.
    std::string TakeSH2JitWorkerWarning() { return std::exchange(m_jitWorkerWarning, std::string{}); }

    /// @brief The JIT executor of the master or slave SH-2, or nullptr if the JIT was never enabled.
    const jit::Executor* GetSH2JitExecutor(bool master) const {
        return master ? m_jitMaster.get() : m_jitSlave.get();
    }

    /// @brief Set deinterlacing enable
    void SetDeinterlacing(bool enable);

    /// @brief Set audio volume percentage
    /// @param percent Volume percentage (0-200)
    void SetAudioVolume(int percent);

    /// @brief Set screen rotation for TATE mode
    /// @param degrees Rotation angle: 0, 90, 180, 270
    void SetRotation(int degrees);

    /// @brief Set overscan crop in pixels from each edge
    /// @param horizontal Total horizontal pixels to crop (from both edges)
    /// @param vertical Total vertical pixels to crop (from both edges)
    void SetOverscanCrop(int horizontal, int vertical);

    // --- Disk control (multi-disc games via M3U) ---

    /// @brief Get the number of discs in the loaded M3U playlist
    size_t GetNumDiscs() const;

    /// @brief Get the current disc index (0-based)
    size_t GetCurrentDiscIndex() const;

    /// @brief Get the filesystem path of a disc by index
    bool GetDiscPath(unsigned index, char* s, size_t len) const;

    /// @brief Get a display label for a disc by index
    bool GetDiscLabel(unsigned index, char* s, size_t len) const;

    /// @brief Open or close the virtual disk tray
    bool SetEjectState(bool ejected);

    /// @brief Check if the virtual disk tray is open
    bool GetEjectState() const;

    /// @brief Eject current disc and insert the disc at the given index
    bool SetDiscIndex(unsigned index);

    /// @brief Add room for a new disc index (for retro_add_image_index)
    bool AddDiscIndex();

    /// @brief Replace the disc at the given index with a new image path
    bool ReplaceDiscIndex(unsigned index, const char* path);

    /// @brief Set the initial disc index from retro_set_initial_image
    bool SetInitialDisc(unsigned index, const char* path);
    
    /// @brief Enable profiling collection (disabled by default). State changes clear samples.
    void SetProfilingEnabled(bool enabled);

    /// @brief Get profiling report
    /// @return Performance profiling data as string
    std::string GetProfilingReport() const { return m_profiler.GetReport(); }
    
    /// @brief Reset profiling data
    void ResetProfiling() { m_profiler.Reset(); }

    /// @brief Read-only access to the profiler (for tools and tests)
    const Profiler& GetProfiler() const { return m_profiler; }
    /// @brief The profiler, for samples timed outside the core (libretro layer: the work RAM view)
    Profiler& GetProfiler() { return m_profiler; }

#ifdef BRIMIR_BUILD_TESTS
    /// @brief Test seam: feed a synthetic native frame (Ymir XBGR8888, tightly packed) through the same path
    /// the VDP frame-complete callback uses. Inline so only test builds that define BRIMIR_BUILD_TESTS see it.
    void TestInjectFrame(const uint32_t* xbgr, uint32_t w, uint32_t h) {
        OnFrameComplete(const_cast<uint32_t*>(xbgr), w, h);
    }

    /// @brief Test seam: the product number LoadGame looks the cartridge up with (empty: the disc's own)
    void TestSetProductNumber(std::string productNumber) { m_testProductNumber = std::move(productNumber); }
#endif

private:
    /// @brief Callback for when VDP completes a frame
    void OnFrameComplete(uint32_t* fb, uint32_t width, uint32_t height);

    /// @brief Callback for when SCSP outputs an audio sample
    void OnAudioSample(int16_t left, int16_t right);

    std::unique_ptr<ymir::Saturn> m_saturn;
    bool m_initialized = false;
    bool m_gameLoaded = false;
    bool m_iplLoaded = false;

    // Video framebuffer info (will be updated from Ymir's VDP)
    unsigned int m_fbWidth = 320;
    unsigned int m_fbHeight = 224;
    unsigned int m_fbPitch = 320 * 4; // XRGB8888 = 4 bytes per pixel
    unsigned int m_pixelFormat = 1;   // XRGB8888
    std::vector<uint32_t> m_framebuffer;  // XRGB8888 converted from Ymir's XBGR
    std::vector<uint32_t> m_displayFramebuffer; // Final output after crop/rotation
    const void* m_displayPointer = nullptr; // Points to final display buffer

    bool m_cpuFrameFromLastRun = false;

    // GPU frame output (SetGpuOutputActive): the raw native frame instead of the CPU transforms
    bool m_gpuOutputActive = false;
    std::vector<uint32_t> m_rawFrame;
    uint32_t m_rawWidth = 0;
    uint32_t m_rawHeight = 0;
    FrameOutputSettings m_rawSettings;

    // The installed Vulkan VDP renderer (owned by the VDP) and its last frame
    gpu::VulkanVDPRenderer* m_vulkanRenderer = nullptr;
    DeviceFrame m_deviceFrame;

    /// @brief Passes the configured VDP1 threading to the Vulkan VDP renderer (the VDP's observers only reach the
    /// software renderer)
    void ApplyVulkanRendererThreading();

    // Screen rotation (TATE mode)
    int m_rotation = 0; // 0, 90, 180, 270

    // Overscan crop in pixels (total from both edges)
    int m_overscanCropH = 0;
    int m_overscanCropV = 0;
    
    // Audio volume (0-200%, scaled as fixed-point 0.0-2.0 in 16.16)
    int m_audioVolume = 100;
    int32_t m_audioVolumeFixed = 65536; // 1.0 in 16.16 fixed-point

    // Audio ring buffer (power of two stereo capacity for fast modulo)
    static constexpr size_t kAudioRingBufferStereoCapacity = 2048;
    AudioRingBuffer<kAudioRingBufferStereoCapacity> m_audioRingBuffer;
    
    // Controller ports: the connected peripheral, the input state kept across frames, and the report mapped from this
    // frame's input (the peripheral report callbacks hand it to the peripheral whenever it is read)
    std::array<PeripheralKind, 2> m_portKind{PeripheralKind::ControlPad, PeripheralKind::ControlPad};
    std::array<PortInputState, 2> m_portInput{};
    std::array<ymir::peripheral::PeripheralReport, 2> m_portReport{};

    /// @brief Connects m_portKind[port] to the Saturn's port and resets its input to released controls
    void ConnectPeripheral(unsigned port);
    /// @brief The report for the port's peripheral from this input
    ymir::peripheral::PeripheralReport MapPortReport(unsigned port, const InputSnapshot& input) const;

    // Peripheral report callbacks
    void OnPeripheralReport1(ymir::peripheral::PeripheralReport& report);
    void OnPeripheralReport2(ymir::peripheral::PeripheralReport& report);
    
    // Last error message from operations (for debugging)
    std::string m_lastError;

    // Uncompressed save-state payload and decoded state (several MB each), kept between calls so
    // that rewind and run-ahead do not reallocate them every frame
    std::vector<uint8_t> m_statePayload;
    std::unique_ptr<ymir::savestate::SaveState> m_stateScratch;
    std::string m_lastStateError;
    std::string m_lastStateWarning;
    
    // Backup RAM (SRAM) data cached for libretro access
    mutable std::vector<uint8_t> m_sramData;
    bool m_sramInitialized = false;
    bool m_sramDataFromFrontend = false;   // True when SetSRAMData or our own .srm load supplied data
    std::filesystem::path m_srmPath;       // Canonical .srm path (also managed by the frontend)
    std::filesystem::path m_smpcBaseDir;   // Directory for system-wide RTC persistent data files
    std::filesystem::path m_smpcPath;      // RTC file of the BIOS region at LoadGame (empty: no persistence)
    mutable bool m_sramCacheDirty = true;  // Track if SRAM cache needs refresh
    mutable uint32_t m_framesSinceLastSRAMSync = 0;  // Frames since last SRAM sync
    bool m_sramFirstLoad = true;  // True until first frame runs (for .srm loading)
    
    // Performance profiling
    Profiler m_profiler;

    // SH-2 JIT executors (one per CPU), created on first enable
    bool m_sh2JitEnabled = false;
    jit::BackendKind m_sh2JitBackend; // set to jit::DefaultBackend() in the constructor
    bool m_sh2JitCompileThread = true;
    // Declared before the executors so it is destroyed after them: their block caches cancel
    // their jobs on it when they are destroyed.
    std::unique_ptr<jit::CompileWorker> m_jitWorker;
    // The worker's thread could not start: compiles stay synchronous until the option or the
    // backend changes. The message waits for the frontend to take and log it.
    bool m_jitWorkerFailed = false;
    std::string m_jitWorkerWarning;
    std::unique_ptr<jit::Executor> m_jitMaster;
    std::unique_ptr<jit::Executor> m_jitSlave;

    // Detaches both CPUs, then destroys the executors and the worker, in that order.
    void DropSH2JitExecutors();
    
    // Cartridge support
    std::filesystem::path m_cartridgePath;  // The cartridge's save file (<game>.cart for DRAM, <game>.bcr for backup)
    bool m_hasCartridge = false;  // True if LoadGame inserted a cartridge
    CartridgeChoice m_cartridgeChoice = CartridgeChoice::Auto;
    std::string m_cartridgeMessage;
    std::string m_cartridgeDescription = "none";
    std::string m_testProductNumber; // TestSetProductNumber
    std::vector<uint8_t> m_cartridgeSaved; // the cartridge's contents as last written to or read from its file
    std::vector<uint8_t> m_cartridgeFailed; // the contents a periodic save failed to write (not retried until changed)
    std::string m_cartridgeSaveError;

    // Accuracy options (SetSH2CacheEmulation, SetCDBlockLLE, SetSCSPStepGranularity)
    bool m_sh2CacheRequested = false;
    bool m_cdblockLLERequested = false;
    bool m_cdblockLLEActive = false;
    bool m_cdblockROMLoaded = false;         // the SH-1 holds a CD Block ROM from the system directory
    std::array<uint8_t, 16> m_cdblockROMHash{}; // its XXH128 hash (sh1::kROMHashSeed), as save states record it
    std::string m_cdblockMessage;
    std::string m_cdblockDescription = "high-level";
    uint32_t m_scspStepGranularity = 0;

    /// @brief Inserts the chosen cartridge for the loaded disc (see SetCartridgeOverride), then hard-resets
    void InsertGameCartridge(const std::filesystem::path& gamePath, const char* save_directory);

    /// @brief The RAM or backup cartridge's contents in its save file's layout (empty for other cartridges)
    std::vector<uint8_t> DumpCartridgeSave() const;
    /// @brief Writes the cartridge's save file; force: even if unchanged since the last write (see FlushCartridgeSave)
    bool SaveCartridgeRAM(bool force);
    void LoadCartridgeRAM();

    /// @brief Copy the cached m_sramData buffer into Ymir's internal backup RAM.
    void WriteSRAMToYmir() const;

    /// @brief Returns the region-qualified SMPC persistent data file path.
    std::filesystem::path GetPersistentSMPCDataPath() const;

    /// @brief Returns the region suffix to use for the SMPC persistent data file.
    std::string GetSMPCRegionSuffix() const;
    /// Region autodetection's preferred order for the loaded BIOS (PreferredAreaOrder); LoadGame, before the disc
    void ApplyPreferredRegionOrder();

    /// @brief Callback invoked by the core when persistent SMPC data should be saved.
    static void OnPersistSMPCData(const ymir::smpc::PersistentSMPCData &data, void *ctx);

    /// @brief Applies m_rtcMode to the Saturn's RTC configuration (see SetRTCMode).
    void ApplyRTCMode();

    RTCMode m_rtcMode = RTCMode::Virtual;

    // Disc loading options
    bool m_discPreload = true; // Preload disc images to RAM by default to avoid streaming hitches

    // Multi-disc support (M3U playlists)
    std::vector<std::filesystem::path> m_discList;
    size_t m_currentDiscIndex = 0;
    std::filesystem::path m_gamePath;
    std::string m_saveDirectory;
    std::string m_systemDirectory;
    bool m_initialDiscSet = false;
    unsigned m_initialDiscIndex = 0;
    std::filesystem::path m_initialDiscPath;
};

} // namespace brimir
