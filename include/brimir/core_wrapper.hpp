// Brimir - Core wrapper for Ymir emulator
// Copyright (C) 2025 Brimir Team
// Licensed under GPL-3.0

#pragma once

#include <memory>
#include <string>
#include <vector>

// Forward declarations to avoid including Ymir headers here
namespace ymir {
struct Saturn;
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
    /// @return true if successful
    bool LoadGame(const char* path);

    /// @brief Unload the current game
    void UnloadGame();

    /// @brief Run one frame of emulation
    void RunFrame();

    /// @brief Reset the emulator
    void Reset();

    /// @brief Load IPL (BIOS) ROM
    /// @param data BIOS data
    /// @param size Size of BIOS data (should be 512KB)
    /// @return true if successful
    bool LoadIPL(const void* data, size_t size);

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

    /// @brief Get audio samples for this frame
    /// @param buffer Buffer to write samples to
    /// @param max_samples Maximum number of stereo samples to write
    /// @return Number of stereo samples written
    size_t GetAudioSamples(int16_t* buffer, size_t max_samples);

    /// @brief Get the size needed for a save state
    /// @return Size in bytes
    size_t GetStateSize() const;

    /// @brief Save state to buffer
    /// @param data Buffer to write state to
    /// @param size Size of buffer
    /// @return true if successful
    bool SaveState(void* data, size_t size);

    /// @brief Load state from buffer
    /// @param data Buffer containing state data
    /// @param size Size of state data
    /// @return true if successful
    bool LoadState(const void* data, size_t size);

    /// @brief Check if a game is currently loaded
    /// @return true if a game is loaded
    bool IsGameLoaded() const { return m_gameLoaded; }

    /// @brief Get the Ymir Saturn instance (for advanced access)
    /// @return Pointer to Saturn instance, or nullptr if not initialized
    ymir::Saturn* GetSaturn() { return m_saturn.get(); }

private:
    std::unique_ptr<ymir::Saturn> m_saturn;
    bool m_initialized = false;
    bool m_gameLoaded = false;

    // Video framebuffer info (will be updated from Ymir's VDP)
    unsigned int m_fbWidth = 320;
    unsigned int m_fbHeight = 224;
    unsigned int m_fbPitch = 320 * 2; // RGB565 = 2 bytes per pixel
    std::vector<uint16_t> m_framebuffer;

    // Audio buffer
    std::vector<int16_t> m_audioBuffer;
};

} // namespace brimir

