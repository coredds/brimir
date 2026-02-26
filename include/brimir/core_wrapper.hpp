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
#include <vector>

#include "brimir/profiler.hpp"

// Include Saturn headers for test access
// Note: Only include in test builds to keep libretro API clean
#ifdef BRIMIR_BUILD_TESTS
#include <brimir/sys/saturn.hpp>
#endif

// Forward declarations to avoid including Ymir headers in libretro builds
namespace brimir {
struct Saturn;

namespace core::config::sys {
enum class VideoStandard;
}

namespace peripheral {
class ControlPad;
struct PeripheralReport;
enum class Button : uint16_t;
}

namespace vdp {
class IVDPRenderer;
}
}

namespace brimir {

/// @brief Wraps the Ymir Saturn emulator for use with libretro
class CoreWrapper {
public:
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

    /// @brief Get pointer to backup RAM (SRAM) data
    /// @return Pointer to SRAM data, or nullptr if not available
    void* GetSRAMData();

    /// @brief Get size of backup RAM (SRAM)
    /// @return Size in bytes
    size_t GetSRAMSize() const;

    /// @brief Write SRAM data back to emulator
    /// @param data Pointer to SRAM data
    /// @param size Size of data in bytes
    /// @return true if successful
    bool SetSRAMData(const void* data, size_t size);
    
    /// @brief Force refresh SRAM from Ymir's backup RAM
    /// This reads from Ymir's .bup file into our buffer
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


    /// @brief Set controller input state for a port
    /// @param port Port number (0 or 1)
    /// @param buttons Button states (libretro button mask)
    void SetControllerState(unsigned int port, uint16_t buttons);

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

    /// @brief Get the pixel format for the framebuffer
    /// @return libretro pixel format (0=0RGB1555, 1=XRGB8888, 2=RGB565)
    unsigned int GetPixelFormat() const;

    /// @brief Get audio samples for this frame
    /// @param buffer Buffer to write samples to
    /// @param max_samples Maximum number of stereo samples to write
    /// @return Number of stereo samples written
    size_t GetAudioSamples(int16_t* buffer, size_t max_samples);

    /// @brief Get the size needed for a save state
    /// @return Size in bytes
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

    /// @brief Set the video standard (NTSC/PAL)
    /// @param standard Video standard to use
    void SetVideoStandard(brimir::core::config::sys::VideoStandard standard);

    /// @brief Get the current video standard
    /// @return Current video standard
    brimir::core::config::sys::VideoStandard GetVideoStandard() const;

    /// @brief Check if the emulator is initialized
    /// @return true if initialized
    bool IsInitialized() const { return m_initialized; }

    /// @brief Check if a game is currently loaded
    /// @return true if a game is loaded
    bool IsGameLoaded() const { return m_gameLoaded; }

    /// @brief Get information about the loaded game
    /// @param title Output buffer for game title (max 256 chars)
    /// @param region Output buffer for region code (max 16 chars)
    /// @return true if game info is available
    bool GetGameInfo(char* title, size_t titleSize, char* region, size_t regionSize) const;

    /// @brief Get the Ymir Saturn instance (for advanced access)
    /// @return Pointer to Saturn instance, or nullptr if not initialized
    brimir::Saturn* GetSaturn() { return m_saturn.get(); }

    /// @brief Set audio interpolation mode
    /// @param mode Interpolation mode: "linear" or "nearest"
    void SetAudioInterpolation(const char* mode);

    /// @brief Set CD read speed multiplier
    /// @param speed Speed multiplier (2-200)
    void SetCDReadSpeed(uint8_t speed);

    /// @brief Set autodetect region from disc
    /// @param enable True to enable autodetect
    void SetAutodetectRegion(bool enable);
    
    /// @brief Set renderer type (Ymir hw layer only supports software)
    /// @param renderer Renderer string: "software"
    void SetRenderer(const char* renderer);
    const char* GetActiveRenderer() const;      // Returns "Software"
    bool IsGPURendererActive() const;           // Always returns false
    const char* GetLastRendererError() const;   // Always returns empty string
    
    /// @brief Set deinterlacing mode
    /// @param enable True to enable deinterlacing
    void SetDeinterlacing(bool enable);
    
    /// @brief Set deinterlacing mode (new optimized modes)
    /// @param mode Mode string: "current", "weave", "blend", "bob", "none"
    void SetDeinterlacingMode(const char* mode);
    
    // GPU-only features (stubbed out with Ymir hw layer)
    void SetHWContext(void* hw_render);         // Not used
    void SetInternalResolution(uint32_t scale); // Not supported
    void SetWireframeMode(bool enable);         // Not supported
    void SetGPUUpscaling(bool enable);          // Not supported
    void SetUpscaleFilter(const char* mode);    // Not supported
    void SetDebanding(bool enable);             // Not supported
    void SetBrightness(float brightness);       // Not supported
    void SetGamma(float gamma);                 // Not supported
    void SetFXAA(bool enable);                  // Not supported
    void SetSharpeningMode(const char* mode);   // Not supported
    
    /// @brief Set horizontal blend filter for interlaced modes
    /// @param enable True to enable horizontal blending in high-res interlaced modes
    void SetHorizontalBlend(bool enable);
    
    /// @brief Set horizontal overscan display
    /// @param enable True to show full horizontal area, false to crop edges
    void SetHorizontalOverscan(bool enable);
    
    /// @brief Set vertical overscan display
    /// @param enable True to show full vertical area, false to crop edges
    void SetVerticalOverscan(bool enable);
    
    /// @brief Get visible framebuffer dimensions after overscan cropping
    /// @param width Output: visible width
    /// @param height Output: visible height
    void GetVisibleResolution(uint32_t& width, uint32_t& height) const;
    
    /// @brief Get profiling report
    /// @return Performance profiling data as string
    std::string GetProfilingReport() const { return m_profiler.GetReport(); }
    
    /// @brief Reset profiling data
    void ResetProfiling() { m_profiler.Reset(); }

private:
    /// @brief Callback for when VDP completes a frame
    void OnFrameComplete(uint32_t* fb, uint32_t width, uint32_t height);

    /// @brief Callback for when SCSP outputs an audio sample
    void OnAudioSample(int16_t left, int16_t right);

    /// @brief Convert XRGB8888 to RGB565
    static uint16_t ConvertXRGB8888toRGB565(uint32_t color);

    std::unique_ptr<brimir::Saturn> m_saturn;
    bool m_initialized = false;
    bool m_gameLoaded = false;
    bool m_iplLoaded = false;

    // Video framebuffer info (will be updated from Ymir's VDP)
    unsigned int m_fbWidth = 320;
    unsigned int m_fbHeight = 224;
    unsigned int m_fbPitch = 320 * 4; // XRGB8888 = 4 bytes per pixel
    unsigned int m_pixelFormat = 1;   // XRGB8888
    std::vector<uint32_t> m_framebuffer;  // XRGB8888 (0x00RRGGBB)
    
    // Audio ring buffer for efficient batching (power of 2 for fast modulo)
    static constexpr size_t kAudioRingBufferSize = 4096;
    std::array<int16_t, kAudioRingBufferSize> m_audioRingBuffer;
    std::atomic<size_t> m_audioRingWritePos{0};
    size_t m_audioRingReadPos = 0;
    
    // Video standard
    brimir::core::config::sys::VideoStandard m_videoStandard;

    // Input devices (raw pointers owned by Saturn's SMPC)
    brimir::peripheral::ControlPad* m_controller1 = nullptr;
    brimir::peripheral::ControlPad* m_controller2 = nullptr;
    
    // Button states for each port (stored for peripheral callback)
    uint16_t m_port1Buttons = 0;
    uint16_t m_port2Buttons = 0;
    
    // Peripheral report callbacks
    void OnPeripheralReport1(brimir::peripheral::PeripheralReport& report);
    void OnPeripheralReport2(brimir::peripheral::PeripheralReport& report);
    
    // Convert libretro button mask to Saturn Button enum
    static brimir::peripheral::Button ConvertLibretroButtons(uint16_t retroButtons);
    
    // Last error message from operations (for debugging)
    std::string m_lastError;
    
    // Backup RAM (SRAM) data cached for libretro access
    mutable std::vector<uint8_t> m_sramData;
    bool m_sramInitialized = false;
    std::filesystem::path m_sramTempPath;  // Temporary file for Ymir's memory-mapped backup RAM
    std::filesystem::path m_smpcPath;      // System-wide RTC persistent data file
    mutable bool m_sramCacheDirty = true;  // Track if SRAM cache needs refresh
    mutable uint32_t m_framesSinceLastSRAMSync = 0;  // Frames since last SRAM sync
    bool m_sramFirstLoad = true;  // True until first frame runs (for .srm loading)
    
    // Performance profiling
    mutable Profiler m_profiler;
    
    // Cartridge support
    std::filesystem::path m_cartridgePath;  // Path to cartridge RAM save file
    bool m_hasCartridge = false;  // True if a cartridge is inserted
    
    /// @brief Load cartridge RAM from file
    bool LoadCartridgeRAM();
    
    /// @brief Save cartridge RAM to file
    void SaveCartridgeRAM();
};

} // namespace brimir


