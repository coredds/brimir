// Brimir - Core wrapper for Ymir emulator
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "brimir/core_wrapper.hpp"

#include <ymir/ymir.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/core/configuration.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_state_common.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_report.hpp>
#include <ymir/state/state.hpp>
#include <ymir/db/game_db.hpp>
#include <ymir/db/rom_cart_db.hpp>
#include <ymir/hw/cart/cart_impl_dram.hpp>
#include <ymir/hw/cart/cart_impl_rom.hpp>
#include <ymir/core/hash.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>

namespace brimir {

CoreWrapper::CoreWrapper()
    : m_videoStandard(ymir::core::config::sys::VideoStandard::NTSC) {
    // Reserve framebuffer space (max Saturn resolution)
    m_framebuffer.resize(704 * 512);
    
    // Audio ring buffer is statically sized (no need to resize)
    m_audioRingBuffer.fill(0);
}

CoreWrapper::~CoreWrapper() {
    Shutdown();
}

bool CoreWrapper::Initialize() {
    if (m_initialized) {
        return true;
    }

    try {
        // Create the Saturn emulator instance
        m_saturn = std::make_unique<ymir::Saturn>();
        
        // NOTE: Ymir requires a file-backed memory-mapped backup RAM
        // We'll set the path later when the game loads (need game name for per-game saves)
        // For now, just mark as uninitialized
        
        // Configure Ymir with threaded VDP for performance (same as standalone app's recommended settings)
        // Threaded VDP is actually FASTER despite async callbacks
        m_saturn->configuration.video.threadedVDP = true;
        m_saturn->configuration.video.threadedDeinterlacer = true;
        m_saturn->configuration.video.includeVDP1InRenderThread = false;
        
        // Disable deinterlacing for maximum performance (same as standalone "Recommended")
        m_saturn->VDP.SetDeinterlaceRender(false);
        
        // Enable transparent mesh for better visuals (same as standalone "Recommended")
        m_saturn->VDP.SetTransparentMeshes(true);
        
        // Set up VDP render callback to capture framebuffer
        auto videoCallback = util::MakeClassMemberOptionalCallback<&CoreWrapper::OnFrameComplete>(this);
        m_saturn->VDP.SetRenderCallback(videoCallback);
        
        // Set up SCSP audio callback to capture audio samples
        auto audioCallback = util::MakeClassMemberOptionalCallback<&CoreWrapper::OnAudioSample>(this);
        m_saturn->SCSP.SetSampleCallback(audioCallback);
        
        // Connect controllers to peripheral ports with input callbacks
        m_controller1 = m_saturn->SMPC.GetPeripheralPort1().ConnectControlPad();
        m_controller2 = m_saturn->SMPC.GetPeripheralPort2().ConnectControlPad();
        
        // Set up peripheral report callbacks for input injection
        auto callback1 = util::MakeClassMemberOptionalCallback<&CoreWrapper::OnPeripheralReport1>(this);
        m_saturn->SMPC.GetPeripheralPort1().SetPeripheralReportCallback(callback1);
        
        auto callback2 = util::MakeClassMemberOptionalCallback<&CoreWrapper::OnPeripheralReport2>(this);
        m_saturn->SMPC.GetPeripheralPort2().SetPeripheralReportCallback(callback2);
        
        m_initialized = true;
        return true;
    } catch (const std::exception&) {
        // Failed to create Saturn instance
        m_saturn.reset();
        m_controller1 = nullptr;
        m_controller2 = nullptr;
        return false;
    }
}

void CoreWrapper::Shutdown() {
    if (!m_initialized) {
        return;
    }

    UnloadGame();
    m_saturn.reset();
    m_initialized = false;
}

bool CoreWrapper::LoadGame(const char* path, const char* save_directory, const char* system_directory) {
    // DON'T touch m_lastError yet - test if that's causing the crash
    
    if (!m_initialized || !m_saturn) {
        return false;
    }

    // Check for nullptr or empty path
    if (!path || path[0] == '\0') {
        return false;
    }
    
    // Re-enable threaded VDP if it was previously disabled during UnloadGame
    try {
        m_saturn->configuration.video.threadedVDP = true;
        m_saturn->configuration.video.threadedDeinterlacer = true;
    } catch (const std::exception& e) {
        m_lastError = std::string("Exception setting threadedVDP: ") + e.what();
        return false;
    } catch (...) {
        m_lastError = "Unknown exception setting threadedVDP";
        return false;
    }

    std::filesystem::path gamePath;
    try {
        gamePath = std::filesystem::path(path);
        
        // Check if file/directory exists
        if (!std::filesystem::exists(gamePath)) {
            return false;
        }
        
        // Set up persistent backup RAM path (Ymir uses memory-mapped files)
        // Use save_directory + game name to create a persistent backup RAM file
        std::filesystem::path gameFileName = gamePath.stem(); // Get filename without extension
        if (save_directory && save_directory[0] != '\0') {
            m_sramTempPath = std::filesystem::path(save_directory) / (gameFileName.string() + ".bup");
        } else {
            // Fallback to temp directory if no save_directory provided
            m_sramTempPath = std::filesystem::temp_directory_path() / (gameFileName.string() + ".bup");
        }
        
        // Ensure parent directory exists
        std::filesystem::create_directories(m_sramTempPath.parent_path());
        
        // Load backup RAM from persistent file (creates if doesn't exist)
        std::error_code error;
        m_saturn->LoadInternalBackupMemoryImage(m_sramTempPath, error);
        if (error) {
            m_lastError = "Failed to load backup RAM from " + m_sramTempPath.string() + ": " + error.message();
            // Don't fail game load - just continue with fresh backup RAM
        }
        
        // Load SMPC persistent data (RTC clock settings!)
        // This is system-wide (not per-game) as the RTC is a console setting, not a game setting
        if (system_directory && system_directory[0] != '\0') {
            m_smpcPath = std::filesystem::path(system_directory) / "brimir_saturn_rtc.smpc";
        } else {
            // Fallback to save directory if no system_directory provided
            m_smpcPath = m_sramTempPath.parent_path() / "brimir_saturn_rtc.smpc";
        }
        m_saturn->SMPC.LoadPersistentDataFrom(m_smpcPath, error);
        if (error) {
            // Not an error - just means first time running
        }
        
        // Immediately read the .bup file into our buffer so RetroArch can see it
        // This ensures the clock settings are visible to RetroArch
        m_sramData = m_saturn->mem.GetInternalBackupRAM().ReadAll();
        m_sramInitialized = true;
        
        // Validate file extension (common Saturn formats)
        std::string extension = gamePath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        
        // Supported formats: .cue, .iso, .chd, .ccd, .mds
        if (extension != ".cue" && extension != ".iso" && extension != ".chd" && 
            extension != ".ccd" && extension != ".mds" && extension != ".bin") {
            // Unknown format, but let Ymir try anyway
        }
        
        // Create a Disc object and load the disc image into it
        ymir::media::Disc disc;
        
        // Clear previous error
        m_lastError.clear();
        
        // Callback for loader messages - capture errors for debugging
        auto loaderCallback = [this](ymir::media::MessageType type, std::string message) {
            // Store error messages
            if (type == ymir::media::MessageType::Error) {
                if (!m_lastError.empty()) {
                    m_lastError += "; ";
                }
                m_lastError += message;
            }
            // Note: Could also capture Warning/Info messages if needed
        };
        
        // Use Ymir's loader to load the disc
        bool success = false;
        try {
            success = ymir::media::LoadDisc(gamePath, disc, false, loaderCallback);
        } catch (const std::exception& e) {
            m_lastError = std::string("Exception during disc load: ") + e.what();
            return false;
        } catch (...) {
            m_lastError = "Unknown exception during disc load";
            return false;
        }
        
        if (!success || disc.sessions.empty()) {
            if (m_lastError.empty()) {
                m_lastError = "Failed to load disc (unknown error)";
            }
            return false;
        }

        // Load the disc into the emulator
        try {
            m_saturn->LoadDisc(std::move(disc));
        } catch (const std::exception& e) {
            m_lastError = std::string("Exception during Saturn LoadDisc: ") + e.what();
            return false;
        } catch (...) {
            m_lastError = "Unknown exception during Saturn LoadDisc";
            return false;
        }
        
        // Close the tray to start execution
        // Note: Some BIOS versions (especially Japanese) require this to properly initialize
        try {
            m_saturn->CloseTray();
        } catch (const std::exception& e) {
            m_lastError = std::string("Exception during CloseTray: ") + e.what();
            return false;
        } catch (...) {
            m_lastError = "Unknown exception during CloseTray";
            return false;
        }
        
        // Re-enable threaded VDP for performance (may have been disabled during previous unload)
        // Do this AFTER CloseTray() to avoid timing issues with Japanese BIOS
        m_saturn->configuration.video.threadedVDP = true;
        m_saturn->configuration.video.threadedDeinterlacer = true;
        
        // Region auto-detection (if enabled in configuration)
        const auto& loadedDisc = m_saturn->GetDisc();
        if (!loadedDisc.sessions.empty()) {
            try {
                m_saturn->AutodetectRegion(loadedDisc.header.compatAreaCode);
            } catch (...) {
                // Non-critical, continue
            }
        }
        
        m_gameLoaded = true;
        // Mark SRAM as dirty to force refresh after game load
        m_sramCacheDirty = true;
        m_sramFirstLoad = true;  // Reset for .srm loading on next run
        return true;
        
    } catch (const std::exception& e) {
        m_lastError = std::string("Exception during game load: ") + e.what();
        m_gameLoaded = false;
        return false;
    } catch (...) {
        m_lastError = "Unknown exception during game load";
        m_gameLoaded = false;
        return false;
    }
}

void CoreWrapper::UnloadGame() {
    if (!m_initialized || !m_saturn) {
        return;
    }
    
    // CRITICAL: Stop threaded VDP FIRST to prevent race conditions
    // This gracefully shuts down the render thread before we do any cleanup
    bool wasThreadedVDP = m_saturn->configuration.video.threadedVDP.Get();
    if (wasThreadedVDP) {
        m_saturn->configuration.video.threadedVDP = false;
        // Give the thread time to shut down cleanly
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // Save SMPC persistent data (RTC clock!) before unloading
    // This preserves the date/time the user set (system-wide, not per-game)
    if (!m_smpcPath.empty()) {
        std::error_code error;
        m_saturn->SMPC.SavePersistentDataTo(m_smpcPath, error);
        if (error) {
            m_lastError = "Failed to save SMPC data: " + error.message();
        }
    }
    
    // Read final SRAM state from Ymir BEFORE unloading
    // This ensures RetroArch can save the latest state (including clock settings)
    if (m_sramInitialized) {
        m_sramData = m_saturn->mem.GetInternalBackupRAM().ReadAll();
    }
    
    // Eject disc
    m_saturn->EjectDisc();
    m_gameLoaded = false;
    
    // Don't clear SRAM buffer - RetroArch needs to save it!
    // m_sramData will persist until next game load
    m_sramInitialized = false;
    m_framesSinceLastSRAMSync = 0;
    m_sramFirstLoad = true;  // Reset for next game load
}

void* CoreWrapper::GetSRAMData() {
    if (!m_initialized || !m_saturn) {
        return nullptr;
    }

    // Initialize buffer on first call (for RetroArch to load .srm into)
    if (m_sramData.empty()) {
        m_sramData.resize(GetSRAMSize());
        // DON'T read from Ymir yet - let RetroArch load .srm first!
        return m_sramData.data();
    }

    // If game is loaded, refresh SRAM from Ymir periodically or when marked dirty
    // This ensures RetroArch always gets the latest state for periodic saves
    if (m_gameLoaded && m_sramInitialized && !m_sramFirstLoad) {
        constexpr uint32_t kSRAMSyncInterval = 300;
        
        if (m_sramCacheDirty || m_framesSinceLastSRAMSync >= kSRAMSyncInterval) {
            m_sramData = m_saturn->mem.GetInternalBackupRAM().ReadAll();
            m_sramCacheDirty = false;
            m_framesSinceLastSRAMSync = 0;
        }
    }
    
    return m_sramData.data();
}

size_t CoreWrapper::GetSRAMSize() const {
    if (!m_initialized || !m_saturn) {
        return 0;
    }

    // Saturn internal backup RAM is 32 KiB
    return m_saturn->mem.GetInternalBackupRAM().Size();
}

void CoreWrapper::RefreshSRAMFromEmulator() {
    if (!m_initialized || !m_saturn || !m_gameLoaded) {
        return;
    }
    
    // Force read from Ymir's .bup file into our buffer
    // This overwrites whatever RetroArch loaded from .srm
    m_sramData = m_saturn->mem.GetInternalBackupRAM().ReadAll();
    m_sramCacheDirty = false;
    m_framesSinceLastSRAMSync = 0;
}

bool CoreWrapper::SetSRAMData(const void* data, size_t size) {
    if (!m_initialized || !m_saturn || !data || size == 0) {
        return false;
    }

    // Get expected SRAM size
    size_t expectedSize = m_saturn->mem.GetInternalBackupRAM().Size();
    if (size != expectedSize) {
        m_lastError = "SRAM size mismatch";
        return false;
    }

    if (m_sramTempPath.empty()) {
        m_lastError = "SRAM temp file path not set";
        return false;
    }

    try {
        // Write the RetroArch .srm data directly to Ymir's memory-mapped file
        std::ofstream file(m_sramTempPath, std::ios::binary | std::ios::in | std::ios::out);
        if (!file) {
            m_lastError = "Failed to open SRAM file for writing";
            return false;
        }

        file.seekp(0);
        file.write(static_cast<const char*>(data), size);
        file.flush();
        file.close();

        m_sramInitialized = true;
        m_sramFirstLoad = false;  // First load complete, can now read from Ymir
        return true;

    } catch (const std::exception& e) {
        m_lastError = std::string("Exception writing SRAM: ") + e.what();
        return false;
    }
}


void CoreWrapper::RunFrame() {
    if (!m_initialized || !m_saturn) {
        return;
    }

    try {
        ScopedTimer timer(m_profiler, "RunFrame_Total");
        
        // Run one frame of emulation
        // VDP callback will update framebuffer via OnFrameComplete()
        // SCSP callback will update audio buffer via OnAudioSample()
        {
            ScopedTimer ymirTimer(m_profiler, "Ymir_RunFrame");
            m_saturn->RunFrame();
        }
        
        // Track frames for SRAM sync optimization
        m_framesSinceLastSRAMSync++;
    } catch (const std::exception& e) {
        // Store exception message for debugging
        m_lastError = std::string("RunFrame exception: ") + e.what();
        // Don't crash the frontend, just stop emulation
        return;
    } catch (...) {
        // Catch any other exceptions
        m_lastError = "RunFrame: Unknown exception";
        return;
    }
}

void CoreWrapper::Reset() {
    if (!m_initialized || !m_saturn) {
        return;
    }

    m_saturn->Reset(false); // Soft reset
}

bool CoreWrapper::LoadIPL(std::span<const uint8_t> data) {
    if (!m_initialized || !m_saturn) {
        return false;
    }

    // Ymir expects exactly 512KB for IPL
    constexpr size_t IPL_SIZE = 512 * 1024;
    if (data.size() != IPL_SIZE) {
        return false;
    }

    try {
        // Ymir's LoadIPL requires non-const span, but we won't modify
        auto mutable_data = const_cast<uint8_t*>(data.data());
        std::span<uint8_t, IPL_SIZE> mutableSpan(mutable_data, IPL_SIZE);
        m_saturn->LoadIPL(mutableSpan);
        m_iplLoaded = true;
        return true;
    } catch (const std::exception& e) {
        (void)e; // Suppress unused variable warning
        m_iplLoaded = false;
        return false;
    }
}

bool CoreWrapper::LoadIPLFromFile(const char* path) {
    if (!path || !m_initialized || !m_saturn) {
        return false;
    }

    constexpr size_t IPL_SIZE = 512 * 1024;
    
    try {
        std::filesystem::path biosPath(path);
        
        // Check if file exists
        if (!std::filesystem::exists(biosPath)) {
            return false;
        }
        
        // Check file size
        auto fileSize = std::filesystem::file_size(biosPath);
        if (fileSize != IPL_SIZE) {
            return false;
        }
        
        // Read BIOS file
        std::ifstream biosFile(biosPath, std::ios::binary);
        if (!biosFile) {
            return false;
        }
        
        std::vector<uint8_t> biosData(IPL_SIZE);
        biosFile.read(reinterpret_cast<char*>(biosData.data()), IPL_SIZE);
        
        if (!biosFile) {
            return false;
        }
        
        // Load into emulator
        return LoadIPL(std::span<const uint8_t>(biosData));
        
    } catch (const std::exception& e) {
        (void)e; // Suppress unused variable warning
        return false;
    }
}


const void* CoreWrapper::GetFramebuffer() const {
    // Return the converted RGB565 framebuffer
    return m_framebuffer.data();
}

unsigned int CoreWrapper::GetFramebufferWidth() const {
    return m_fbWidth;
}

unsigned int CoreWrapper::GetFramebufferHeight() const {
    return m_fbHeight;
}

unsigned int CoreWrapper::GetFramebufferPitch() const {
    return m_fbPitch;
}

unsigned int CoreWrapper::GetPixelFormat() const {
    return m_pixelFormat;  // 2 = RGB565
}

size_t CoreWrapper::GetAudioSamples(int16_t* buffer, size_t max_samples) {
    if (!m_initialized || !m_saturn || !buffer) {
        return 0;
    }

    // Get current write position
    size_t writePos = m_audioRingWritePos.load(std::memory_order_acquire);
    
    // Calculate available samples in ring buffer
    // Both positions are in int16_t units, so divide by 2 for stereo pairs
    size_t available = (writePos >= m_audioRingReadPos) 
        ? (writePos - m_audioRingReadPos) / 2
        : ((kAudioRingBufferSize - m_audioRingReadPos) + writePos) / 2;
    
    size_t samples_to_copy = std::min(available, max_samples);
    
    if (samples_to_copy == 0) {
        return 0;
    }
    
    // Copy from ring buffer (handle wrap-around)
    size_t samples_copied = 0;
    while (samples_copied < samples_to_copy) {
        buffer[samples_copied * 2] = m_audioRingBuffer[m_audioRingReadPos];
        buffer[samples_copied * 2 + 1] = m_audioRingBuffer[m_audioRingReadPos + 1];
        
        m_audioRingReadPos = (m_audioRingReadPos + 2) & (kAudioRingBufferSize - 1);
        samples_copied++;
    }
    
    return samples_to_copy;
}

size_t CoreWrapper::GetStateSize() const {
    if (!m_initialized || !m_saturn) {
        return 0;
    }

    // Return the size of Ymir's State structure
    // This includes all emulator state (CPU, VDP, SCSP, memory, etc.)
    return sizeof(ymir::state::State);
}

bool CoreWrapper::SaveState(void* data, size_t size) {
    if (!m_initialized || !m_saturn || !data) {
        return false;
    }
    
    // Verify buffer is large enough
    size_t requiredSize = GetStateSize();
    if (size < requiredSize) {
        return false;
    }

    try {
        // Create a State object on the heap (too large for stack)
        auto state = std::make_unique<ymir::state::State>();
        m_saturn->SaveState(*state);
        
        // Serialize state to buffer (simple memcpy for now)
        std::memcpy(data, state.get(), requiredSize);
        
        return true;
    } catch (const std::exception& e) {
        (void)e; // Suppress unused variable warning
        return false;
    }
}

bool CoreWrapper::LoadState(const void* data, size_t size) {
    if (!m_initialized || !m_saturn || !data) {
        return false;
    }
    
    // Verify buffer size matches expected state size
    size_t requiredSize = GetStateSize();
    if (size < requiredSize) {
        return false;
    }

    try {
        // Deserialize state from buffer (allocate on heap - too large for stack)
        auto state = std::make_unique<ymir::state::State>();
        std::memcpy(state.get(), data, requiredSize);
        
        // Load state into emulator
        // skipROMChecks = true to allow loading states with different BIOS
        bool success = m_saturn->LoadState(*state, true);
        
        return success;
    } catch (const std::exception& e) {
        (void)e; // Suppress unused variable warning
        return false;
    }
}

void CoreWrapper::SetVideoStandard(ymir::core::config::sys::VideoStandard standard) {
    m_videoStandard = standard;
    if (m_initialized && m_saturn) {
        // TODO: Apply to Ymir Saturn instance
        // m_saturn->GetConfig().sys.videoStandard = standard;
    }
}

ymir::core::config::sys::VideoStandard CoreWrapper::GetVideoStandard() const {
    return m_videoStandard;
}

void CoreWrapper::OnFrameComplete(uint32_t* fb, uint32_t width, uint32_t height) {
    ScopedTimer timer(m_profiler, "OnFrameComplete_Total");
    
    if (!fb || width == 0 || height == 0) {
        return;
    }

    // Update dimensions
    m_fbWidth = width;
    m_fbHeight = height;
    m_fbPitch = width * 2; // RGB565 is 2 bytes per pixel

    // Resize framebuffer if needed
    size_t pixelCount = static_cast<size_t>(width) * height;
    if (m_framebuffer.size() < pixelCount) {
        m_framebuffer.resize(pixelCount);
    }

    // Convert from XBGR8888 to RGB565
    {
        ScopedTimer convTimer(m_profiler, "PixelConversion");
        
        // Use manual loop unrolling for better performance on high-res frames
        const uint32_t* src = fb;
        uint16_t* dst = m_framebuffer.data();
        
        // Process 4 pixels at a time (manual unrolling for better ILP)
        size_t i = 0;
        const size_t unroll4 = pixelCount & ~3;  // Round down to multiple of 4
        
        for (; i < unroll4; i += 4) {
            // Process 4 pixels with independent operations (allows CPU to parallelize)
            uint32_t p0 = src[i + 0];
            uint32_t p1 = src[i + 1];
            uint32_t p2 = src[i + 2];
            uint32_t p3 = src[i + 3];
            
            // Convert each pixel independently (compiler can vectorize this)
            dst[i + 0] = ((p0 & 0x0000F8) << 8) | ((p0 & 0x00FC00) >> 5) | ((p0 & 0xF80000) >> 19);
            dst[i + 1] = ((p1 & 0x0000F8) << 8) | ((p1 & 0x00FC00) >> 5) | ((p1 & 0xF80000) >> 19);
            dst[i + 2] = ((p2 & 0x0000F8) << 8) | ((p2 & 0x00FC00) >> 5) | ((p2 & 0xF80000) >> 19);
            dst[i + 3] = ((p3 & 0x0000F8) << 8) | ((p3 & 0x00FC00) >> 5) | ((p3 & 0xF80000) >> 19);
        }
        
        // Handle remaining pixels (0-3)
        for (; i < pixelCount; ++i) {
            uint32_t pixel = src[i];
            dst[i] = ((pixel & 0x0000F8) << 8) | ((pixel & 0x00FC00) >> 5) | ((pixel & 0xF80000) >> 19);
        }
    }
}

uint16_t CoreWrapper::ConvertXRGB8888toRGB565(uint32_t color) {
    // Ymir outputs in XBGR8888 format: 0xXXBBGGRR
    // Note: Byte order is BGR, not RGB!
    uint8_t b = (color >> 16) & 0xFF;  // Blue in high byte
    uint8_t g = (color >> 8) & 0xFF;   // Green in middle byte
    uint8_t r = color & 0xFF;          // Red in low byte

    // Convert 8-bit channels to RGB565
    // RGB565: RRRRRGGG GGGBBBBB
    uint16_t r5 = (r >> 3) & 0x1F;
    uint16_t g6 = (g >> 2) & 0x3F;
    uint16_t b5 = (b >> 3) & 0x1F;

    return (r5 << 11) | (g6 << 5) | b5;
}

void CoreWrapper::OnAudioSample(int16_t left, int16_t right) {
    // Efficient ring buffer implementation - no bounds checks or allocations!
    // Write position is atomic for thread safety (though SCSP is single-threaded)
    size_t writePos = m_audioRingWritePos.load(std::memory_order_relaxed);
    
    m_audioRingBuffer[writePos] = left;
    m_audioRingBuffer[writePos + 1] = right;
    
    // Fast modulo using bitwise AND (kAudioRingBufferSize is power of 2)
    m_audioRingWritePos.store((writePos + 2) & (kAudioRingBufferSize - 1), std::memory_order_release);
}

void CoreWrapper::SetControllerState(unsigned int port, uint16_t buttons) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    // Store button state for the specified port
    // The peripheral callback will read these when the emulator needs input
    if (port == 0) {
        m_port1Buttons = buttons;
    } else if (port == 1) {
        m_port2Buttons = buttons;
    }
}

bool CoreWrapper::GetGameInfo(char* title, size_t titleSize, char* region, size_t regionSize) const {
    if (!m_initialized || !m_saturn || !m_gameLoaded) {
        return false;
    }

    if (!title || titleSize == 0 || !region || regionSize == 0) {
        return false;
    }

    try {
        const auto& disc = m_saturn->GetDisc();
        
        // Copy game title
        const auto& gameTitle = disc.header.gameTitle;
        size_t titleLen = std::min(gameTitle.length(), titleSize - 1);
        std::memcpy(title, gameTitle.c_str(), titleLen);
        title[titleLen] = '\0';
        
        // Convert area code to string
        std::string regionStr;
        auto areaCode = disc.header.compatAreaCode;
        
        // Area codes can be combined (e.g., JUE for all regions)
        if ((areaCode & ymir::media::AreaCode::Japan) != ymir::media::AreaCode::None) {
            regionStr += "J";
        }
        if ((areaCode & ymir::media::AreaCode::NorthAmerica) != ymir::media::AreaCode::None) {
            regionStr += "U";
        }
        if ((areaCode & ymir::media::AreaCode::EuropePAL) != ymir::media::AreaCode::None) {
            regionStr += "E";
        }
        if ((areaCode & ymir::media::AreaCode::AsiaPAL) != ymir::media::AreaCode::None) {
            regionStr += "A";
        }
        if ((areaCode & ymir::media::AreaCode::AsiaNTSC) != ymir::media::AreaCode::None) {
            regionStr += "T";
        }
        
        if (regionStr.empty()) {
            regionStr = "Unknown";
        }
        
        size_t regionLen = std::min(regionStr.length(), regionSize - 1);
        std::memcpy(region, regionStr.c_str(), regionLen);
        region[regionLen] = '\0';
        
        return true;
        
    } catch (const std::exception& e) {
        (void)e; // Suppress unused variable warning
        return false;
    }
}

void CoreWrapper::OnPeripheralReport1(ymir::peripheral::PeripheralReport& report) {
    if (report.type == ymir::peripheral::PeripheralType::ControlPad) {
        // Convert libretro button mask to Saturn button states
        report.report.controlPad.buttons = ConvertLibretroButtons(m_port1Buttons);
    }
}

void CoreWrapper::OnPeripheralReport2(ymir::peripheral::PeripheralReport& report) {
    if (report.type == ymir::peripheral::PeripheralType::ControlPad) {
        // Convert libretro button mask to Saturn button states
        report.report.controlPad.buttons = ConvertLibretroButtons(m_port2Buttons);
    }
}

ymir::peripheral::Button CoreWrapper::ConvertLibretroButtons(uint16_t retroButtons) {
    // libretro button indices (standard mapping)
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_B = 0;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_Y = 1;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_SELECT = 2;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_START = 3;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_UP = 4;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_DOWN = 5;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_LEFT = 6;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_RIGHT = 7;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_A = 8;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_X = 9;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_L = 10;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_R = 11;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_L2 = 12;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_R2 = 13;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_L3 = 14;
    constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_R3 = 15;
    
    // Start with all buttons released (1 = released in Saturn)
    ymir::peripheral::Button saturnButtons = ymir::peripheral::Button::Default;
    
    // Map libretro buttons to Saturn buttons (0 = pressed in Saturn)
    // Note: libretro uses bit mask (1 = pressed), Saturn uses inverted (0 = pressed)
    
    // D-pad
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_UP)) {
        saturnButtons &= ~ymir::peripheral::Button::Up;
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)) {
        saturnButtons &= ~ymir::peripheral::Button::Down;
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)) {
        saturnButtons &= ~ymir::peripheral::Button::Left;
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)) {
        saturnButtons &= ~ymir::peripheral::Button::Right;
    }
    
    // Face buttons (map to Saturn's 6-button layout)
    // Saturn: A B C X Y Z
    // Libretro: B A Y X (SNES layout)
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_B)) {
        saturnButtons &= ~ymir::peripheral::Button::A;  // B -> A
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_A)) {
        saturnButtons &= ~ymir::peripheral::Button::B;  // A -> B
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_Y)) {
        saturnButtons &= ~ymir::peripheral::Button::C;  // Y -> C
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_X)) {
        saturnButtons &= ~ymir::peripheral::Button::X;  // X -> X
    }
    // L2/R2 for Y/Z
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_L2)) {
        saturnButtons &= ~ymir::peripheral::Button::Y;  // L2 -> Y
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_R2)) {
        saturnButtons &= ~ymir::peripheral::Button::Z;  // R2 -> Z
    }
    
    // Shoulder buttons
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_L)) {
        saturnButtons &= ~ymir::peripheral::Button::L;
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_R)) {
        saturnButtons &= ~ymir::peripheral::Button::R;
    }
    
    // Start button
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_START)) {
        saturnButtons &= ~ymir::peripheral::Button::Start;
    }
    
    return saturnButtons;
}

void CoreWrapper::SetAudioInterpolation(const char* mode) {
    if (!m_initialized || !m_saturn || !mode) {
        return;
    }

    using InterpolationMode = ymir::core::config::audio::SampleInterpolationMode;
    
    if (strcmp(mode, "linear") == 0) {
        m_saturn->configuration.audio.interpolation = InterpolationMode::Linear;
    } else if (strcmp(mode, "nearest") == 0) {
        m_saturn->configuration.audio.interpolation = InterpolationMode::NearestNeighbor;
    }
}

void CoreWrapper::SetCDReadSpeed(uint8_t speed) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    // Clamp to valid range (2-200)
    if (speed < 2) speed = 2;
    if (speed > 200) speed = 200;

    m_saturn->configuration.cdblock.readSpeedFactor = speed;
}

void CoreWrapper::SetAutodetectRegion(bool enable) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    m_saturn->configuration.system.autodetectRegion = enable;
}


} // namespace brimir

