// Brimir - Core wrapper for Ymir emulator
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "brimir/core_wrapper.hpp"

#include <brimir/brimir.hpp>
#include <brimir/media/loader/loader.hpp>
#include <brimir/core/configuration.hpp>
#include <brimir/hw/smpc/peripheral/peripheral_state_common.hpp>
#include <brimir/hw/smpc/peripheral/peripheral_report.hpp>
#include <brimir/state/state.hpp>
#include <brimir/db/game_db.hpp>
#include <brimir/db/rom_cart_db.hpp>
#include <brimir/hw/cart/cart_impl_dram.hpp>
#include <brimir/hw/cart/cart_impl_rom.hpp>
#include <brimir/core/hash.hpp>
#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <brimir/hw/vdp/gpu/vulkan_renderer.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>

// SIMD intrinsics
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>  // SSE2
#endif

// Undefine any Windows macros that might interfere
#ifdef DRAM8Mbit
#undef DRAM8Mbit
#endif
#ifdef DRAM32Mbit
#undef DRAM32Mbit
#endif
#ifdef DRAM48Mbit
#undef DRAM48Mbit
#endif

namespace brimir {

CoreWrapper::CoreWrapper()
    : m_videoStandard(brimir::core::config::sys::VideoStandard::NTSC) {
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
        m_saturn = std::make_unique<brimir::Saturn>();
        
        // NOTE: Ymir requires a file-backed memory-mapped backup RAM
        // We'll set the path later when the game loads (need game name for per-game saves)
        // For now, just mark as uninitialized
        
        // Configure Brimir with optimized settings for libretro
        // Threaded VDP provides better frame pacing and async rendering
        m_saturn->configuration.video.threadedVDP = true;
        // Note: threadedDeinterlacer will be set by SetDeinterlaceMode() below
        m_saturn->configuration.video.includeVDP1InRenderThread = false;
        
        // Enable deinterlacing with Bob mode for optimal libretro performance
        // Bob mode: 60 FPS, no scanlines, no shader required - ideal for RetroArch
        // Many games use interlaced hi-res menus (Panzer Dragoon Zwei, Grandia, etc)
        // Note: SetDeinterlaceMode() controls both m_deinterlaceRender and m_threadedDeinterlacer
        m_saturn->VDP.SetDeinterlaceMode(brimir::vdp::DeinterlaceMode::Bob);
        
        // Enable transparent mesh for accurate VDP1 rendering
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
        // Note: threadedDeinterlacer is controlled by DeinterlaceMode, don't override it here
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
        brimir::media::Disc disc;
        
        // Clear previous error
        m_lastError.clear();
        
        // Callback for loader messages - capture errors for debugging
        auto loaderCallback = [this](brimir::media::MessageType type, std::string message) {
            // Store error messages
            if (type == brimir::media::MessageType::Error) {
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
            success = brimir::media::LoadDisc(gamePath, disc, false, loaderCallback);
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

        // Load the disc into the emulator first
        try {
            m_saturn->LoadDisc(std::move(disc));
        } catch (const std::exception& e) {
            m_lastError = std::string("Exception during Saturn LoadDisc: ") + e.what();
            return false;
        } catch (...) {
            m_lastError = "Unknown exception during Saturn LoadDisc";
            return false;
        }
        
        // Detect and insert required cartridge AFTER loading disc but BEFORE closing tray
        // The cartridge must be inserted before the BIOS initializes
        const auto& discForCartridge = m_saturn->GetDisc();
        if (!discForCartridge.sessions.empty()) {
            // Get game info from Ymir's database
            const brimir::db::GameInfo* gameInfo = brimir::db::GetGameInfo(
                discForCartridge.header.productNumber,
                m_saturn->GetDiscHash()
            );
            
            if (gameInfo && gameInfo->GetCartridge() != brimir::db::Cartridge::None) {
                // Set up cartridge RAM save path
                std::filesystem::path gameFileName = gamePath.stem();
                if (save_directory && save_directory[0] != '\0') {
                    m_cartridgePath = std::filesystem::path(save_directory) / (gameFileName.string() + ".cart");
                } else {
                    m_cartridgePath = std::filesystem::path(save_directory) / (gameFileName.string() + ".cart");
                }
                
                // Insert the appropriate cartridge
                try {
                    switch (gameInfo->GetCartridge()) {
                    case brimir::db::Cartridge::DRAM8Mbit:
                        m_saturn->InsertCartridge<brimir::cart::DRAM8MbitCartridge>();
                        m_hasCartridge = true;
                        // TODO: LoadCartridgeRAM();  // Load saved RAM if exists
                        break;
                    case brimir::db::Cartridge::DRAM32Mbit:
                        m_saturn->InsertCartridge<brimir::cart::DRAM32MbitCartridge>();
                        m_hasCartridge = true;
                        // TODO: LoadCartridgeRAM();  // Load saved RAM if exists
                        break;
                    case brimir::db::Cartridge::DRAM48Mbit:
                        m_saturn->InsertCartridge<brimir::cart::DRAM48MbitCartridge>();
                        m_hasCartridge = true;
                        // TODO: LoadCartridgeRAM();  // Load saved RAM if exists
                        break;
                    default:
                        // Other cartridge types (ROM, BackupRAM) not yet supported
                        break;
                    }
                    
                    // Do a HARD reset after inserting cartridge to force BIOS reboot
                    // This is necessary because the BIOS has already initialized
                    m_saturn->Reset(true);
                    
                } catch (const std::exception& e) {
                    m_lastError = std::string("Exception inserting cartridge: ") + e.what();
                    // Don't fail - game might still work without cartridge
                } catch (...) {
                    m_lastError = "Unknown exception inserting cartridge";
                    // Don't fail - game might still work without cartridge
                }
            }
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
    
    // TODO: Save cartridge RAM if present
    // if (m_hasCartridge) {
    //     SaveCartridgeRAM();
    // }
    
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
    
    // Reset cartridge state
    m_hasCartridge = false;
    m_cartridgePath.clear();
    
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
    // Return GPU upscaled framebuffer directly (no copy), or software framebuffer
    if (m_upscaledFrameReady && m_gpuRenderer) {
        const void* upscaled = m_gpuRenderer->GetUpscaledFramebuffer();
        if (upscaled) return upscaled;
    }
    return m_framebuffer.data();
}

unsigned int CoreWrapper::GetFramebufferWidth() const {
    if (m_upscaledFrameReady && m_upscaledWidth > 0) {
        return m_upscaledWidth;
    }
    return m_fbWidth;
}

unsigned int CoreWrapper::GetFramebufferHeight() const {
    if (m_upscaledFrameReady && m_upscaledHeight > 0) {
        return m_upscaledHeight;
    }
    return m_fbHeight;
}

unsigned int CoreWrapper::GetFramebufferPitch() const {
    if (m_upscaledFrameReady && m_upscaledPitch > 0) {
        return m_upscaledPitch;  // Already in bytes (XRGB8888)
    }
    return m_fbPitch;
}

unsigned int CoreWrapper::GetPixelFormat() const {
    return m_pixelFormat;  // 1 = XRGB8888
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
    return sizeof(brimir::state::State);
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
        auto state = std::make_unique<brimir::state::State>();
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
        auto state = std::make_unique<brimir::state::State>();
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

void CoreWrapper::SetVideoStandard(brimir::core::config::sys::VideoStandard standard) {
    m_videoStandard = standard;
    if (m_initialized && m_saturn) {
        // TODO: Apply to Ymir Saturn instance
        // m_saturn->GetConfig().sys.videoStandard = standard;
    }
}

brimir::core::config::sys::VideoStandard CoreWrapper::GetVideoStandard() const {
    return m_videoStandard;
}

void CoreWrapper::OnFrameComplete(uint32_t* fb, uint32_t width, uint32_t height) {
    ScopedTimer timer(m_profiler, "OnFrameComplete_Total");
    
    if (!fb || width == 0 || height == 0) {
        return;
    }

    // Get overscan cropping parameters
    const uint32_t visibleWidth = m_saturn->VDP.GetVisibleWidth();
    const uint32_t visibleHeight = m_saturn->VDP.GetVisibleHeight();
    const uint32_t xOffset = m_saturn->VDP.GetHOverscanOffset();
    const uint32_t yOffset = m_saturn->VDP.GetVOverscanOffset();
    
    // Update output dimensions (visible area after cropping)
    m_fbWidth = visibleWidth;
    m_fbHeight = visibleHeight;
    m_fbPitch = visibleWidth * 4; // XRGB8888 = 4 bytes per pixel

    // Resize framebuffer if needed
    size_t pixelCount = static_cast<size_t>(visibleWidth) * visibleHeight;
    if (m_framebuffer.size() < pixelCount) {
        m_framebuffer.resize(pixelCount);
    }

    // Convert from VDP's XBGR8888 (0x00BBGGRR) to libretro XRGB8888 (0x00RRGGBB)
    // This is a lossless R<->B channel swap, replacing the old lossy RGB565 conversion
    {
        ScopedTimer convTimer(m_profiler, "PixelConversion");
        
        uint32_t* dst = m_framebuffer.data();
        
        // Process line by line to handle overscan cropping
        for (uint32_t y = 0; y < visibleHeight; ++y) {
            const uint32_t srcY = y + yOffset;
            const uint32_t* srcLine = fb + (srcY * width) + xOffset;
            uint32_t* dstLine = dst + (y * visibleWidth);
            
            size_t i = 0;
            
#if defined(_M_X64) || defined(__x86_64__)
    #if defined(__SSE2__) || defined(_M_X64)
            // SSE2: Process 4 pixels at a time
            // XBGR8888 (0x00BBGGRR) -> XRGB8888 (0x00RRGGBB): swap R and B channels
            const __m128i maskRB = _mm_set1_epi32(0x00FF00FF);  // R and B channel mask
            const __m128i maskG  = _mm_set1_epi32(0x0000FF00);  // G channel mask
            
            for (; i + 4 <= visibleWidth; i += 4) {
                __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&srcLine[i]));
                
                // Extract R (bits 0-7) and B (bits 16-23)
                __m128i rb = _mm_and_si128(pixels, maskRB);
                // Swap R and B: shift R left by 16, B right by 16
                __m128i rb_swapped = _mm_or_si128(_mm_slli_epi32(rb, 16), _mm_srli_epi32(rb, 16));
                // Keep only the valid swapped bits (mask out overflow from shift)
                rb_swapped = _mm_and_si128(rb_swapped, maskRB);
                // Keep G channel as-is
                __m128i g = _mm_and_si128(pixels, maskG);
                // Combine: swapped R/B + original G
                __m128i result = _mm_or_si128(rb_swapped, g);
                
                _mm_storeu_si128(reinterpret_cast<__m128i*>(&dstLine[i]), result);
            }
    #endif
#endif
            
            // Scalar fallback for remaining pixels
            for (; i < visibleWidth; ++i) {
                uint32_t pixel = srcLine[i];
                // Swap R (bits 0-7) and B (bits 16-23), keep G (bits 8-15)
                uint32_t r = pixel & 0x000000FF;
                uint32_t g = pixel & 0x0000FF00;
                uint32_t b = pixel & 0x00FF0000;
                dstLine[i] = (r << 16) | g | (b >> 16);
            }
        }
    }
    
    // GPU upscaling: upload software-rendered frame to GPU and upscale
    m_upscaledFrameReady = false;
    if (m_useGPUUpscaling && m_gpuRenderer && m_internalScale > 1) {
        ScopedTimer gpuTimer(m_profiler, "GPUUpscale");
        
        // Upload the XRGB8888 framebuffer to GPU
        m_gpuRenderer->UploadSoftwareFramebuffer(
            m_framebuffer.data(), visibleWidth, visibleHeight, visibleWidth * 4);
        
        // Render upscaled version
        if (m_gpuRenderer->RenderUpscaled()) {
            // Use renderer's buffer directly (valid until next RenderUpscaled call)
            const void* upscaledData = m_gpuRenderer->GetUpscaledFramebuffer();
            if (upscaledData) {
                m_upscaledWidth = m_gpuRenderer->GetUpscaledWidth();
                m_upscaledHeight = m_gpuRenderer->GetUpscaledHeight();
                m_upscaledPitch = m_gpuRenderer->GetUpscaledPitch();
                m_upscaledFrameReady = true;
            }
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
        if ((areaCode & brimir::media::AreaCode::Japan) != brimir::media::AreaCode::None) {
            regionStr += "J";
        }
        if ((areaCode & brimir::media::AreaCode::NorthAmerica) != brimir::media::AreaCode::None) {
            regionStr += "U";
        }
        if ((areaCode & brimir::media::AreaCode::EuropePAL) != brimir::media::AreaCode::None) {
            regionStr += "E";
        }
        if ((areaCode & brimir::media::AreaCode::AsiaPAL) != brimir::media::AreaCode::None) {
            regionStr += "A";
        }
        if ((areaCode & brimir::media::AreaCode::AsiaNTSC) != brimir::media::AreaCode::None) {
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

void CoreWrapper::OnPeripheralReport1(brimir::peripheral::PeripheralReport& report) {
    if (report.type == brimir::peripheral::PeripheralType::ControlPad) {
        // Convert libretro button mask to Saturn button states
        report.report.controlPad.buttons = ConvertLibretroButtons(m_port1Buttons);
    }
}

void CoreWrapper::OnPeripheralReport2(brimir::peripheral::PeripheralReport& report) {
    if (report.type == brimir::peripheral::PeripheralType::ControlPad) {
        // Convert libretro button mask to Saturn button states
        report.report.controlPad.buttons = ConvertLibretroButtons(m_port2Buttons);
    }
}

brimir::peripheral::Button CoreWrapper::ConvertLibretroButtons(uint16_t retroButtons) {
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
    brimir::peripheral::Button saturnButtons = brimir::peripheral::Button::Default;
    
    // Map libretro buttons to Saturn buttons (0 = pressed in Saturn)
    // Note: libretro uses bit mask (1 = pressed), Saturn uses inverted (0 = pressed)
    
    // D-pad
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_UP)) {
        saturnButtons &= ~brimir::peripheral::Button::Up;
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)) {
        saturnButtons &= ~brimir::peripheral::Button::Down;
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)) {
        saturnButtons &= ~brimir::peripheral::Button::Left;
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)) {
        saturnButtons &= ~brimir::peripheral::Button::Right;
    }
    
    // Face buttons (map to Saturn's 6-button layout)
    // Saturn: A B C X Y Z
    // Libretro: B A Y X (SNES layout)
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_B)) {
        saturnButtons &= ~brimir::peripheral::Button::A;  // B -> A
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_A)) {
        saturnButtons &= ~brimir::peripheral::Button::B;  // A -> B
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_Y)) {
        saturnButtons &= ~brimir::peripheral::Button::C;  // Y -> C
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_X)) {
        saturnButtons &= ~brimir::peripheral::Button::X;  // X -> X
    }
    // L2/R2 for Y/Z
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_L2)) {
        saturnButtons &= ~brimir::peripheral::Button::Y;  // L2 -> Y
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_R2)) {
        saturnButtons &= ~brimir::peripheral::Button::Z;  // R2 -> Z
    }
    
    // Shoulder buttons
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_L)) {
        saturnButtons &= ~brimir::peripheral::Button::L;
    }
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_R)) {
        saturnButtons &= ~brimir::peripheral::Button::R;
    }
    
    // Start button
    if (retroButtons & (1 << RETRO_DEVICE_ID_JOYPAD_START)) {
        saturnButtons &= ~brimir::peripheral::Button::Start;
    }
    
    return saturnButtons;
}

void CoreWrapper::SetAudioInterpolation(const char* mode) {
    if (!m_initialized || !m_saturn || !mode) {
        return;
    }

    using InterpolationMode = brimir::core::config::audio::SampleInterpolationMode;
    
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

void CoreWrapper::SetHWContext(void* hw_render) {
    m_hwRenderCallback = hw_render;
}

void CoreWrapper::SetRenderer(const char* renderer) {
    if (!m_initialized || !m_saturn || !renderer) {
        return;
    }

    brimir::vdp::RendererType type = brimir::vdp::RendererType::Software;
    if (strcmp(renderer, "vulkan") == 0) {
        type = brimir::vdp::RendererType::Vulkan;
    } else if (strcmp(renderer, "software") == 0) {
        type = brimir::vdp::RendererType::Software;
    } else {
        type = brimir::vdp::RendererType::Software;
    }

    // Check availability
    if (type != brimir::vdp::RendererType::Software) {
        bool available = brimir::vdp::IsRendererAvailable(type);
        if (!available) {
            type = brimir::vdp::RendererType::Software;
        }
    }

    if (type == brimir::vdp::RendererType::Software) {
        m_gpuRenderer.reset(); // Destroy GPU renderer if switching to software
        m_saturn->VDP.SetGPURenderer(nullptr, false);
    } else {
        // Try to create GPU renderer
        m_gpuRenderer = brimir::vdp::CreateRenderer(type);
        
        if (!m_gpuRenderer) {
            // CreateRenderer returned null
            m_saturn->VDP.SetGPURenderer(nullptr, false);
            return;
        }
        
        // Pass hardware context if available
        if (m_hwRenderCallback) {
            // The renderer will handle the type internally
            m_gpuRenderer->SetHWContext(m_hwRenderCallback);
        }
        
        // Try to initialize
        bool initSuccess = m_gpuRenderer->Initialize();
        
        if (initSuccess) {
            m_saturn->VDP.SetGPURenderer(m_gpuRenderer.get(), true);
        } else {
            // Initialize failed - store error for libretro to log
            m_lastRendererError = m_gpuRenderer->GetLastError();
            m_gpuRenderer.reset();
            m_saturn->VDP.SetGPURenderer(nullptr, false);
        }
    }
}

void CoreWrapper::SetDeinterlacing(bool enable) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    m_saturn->VDP.SetDeinterlaceRender(enable);
}

void CoreWrapper::SetHorizontalBlend(bool enable) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    m_saturn->VDP.SetHorizontalBlend(enable);
}

void CoreWrapper::SetHorizontalOverscan(bool enable) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    m_saturn->VDP.SetHorizontalOverscan(enable);
}

void CoreWrapper::SetInternalResolution(uint32_t scale) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    // Clamp to valid range
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    
    // Store scale locally for CPU upscaling
    m_internalScale = scale;

    // Check if GPU renderer is active (just check our local pointer)
    bool gpuActive = (m_gpuRenderer != nullptr);
    
    if (gpuActive) {
        m_gpuRenderer->SetInternalScale(scale);
    }
    
    // Auto-enable upscaling when scale > 1
    if (scale > 1) {
        m_useGPUUpscaling = true;
    } else {
        m_useGPUUpscaling = false;
    }
    // Note: Logging happens in libretro layer
}

void CoreWrapper::SetGPUUpscaling(bool enable) {
    if (!m_initialized) {
        return;
    }
    
    // Only enable if we have a GPU renderer
    if (enable && m_gpuRenderer) {
        m_useGPUUpscaling = true;
    } else {
        m_useGPUUpscaling = false;
        m_upscaledFrameReady = false;
    }
    // If GPU not active, silently ignore (libretro will log warning)
}

void CoreWrapper::SetUpscaleFilter(const char* mode) {
    if (!mode) return;
    
    uint32_t filterMode = 2; // default: sharp bilinear
    if (std::strcmp(mode, "nearest") == 0) {
        filterMode = 0;
    } else if (std::strcmp(mode, "bilinear") == 0) {
        filterMode = 1;
    } else if (std::strcmp(mode, "sharp_bilinear") == 0) {
        filterMode = 2;
    }
    
    m_upscaleFilter = filterMode;
    if (m_gpuRenderer) {
        m_gpuRenderer->SetUpscaleFilter(filterMode);
    }
}

void CoreWrapper::SetScanlines(bool enable) {
    m_scanlines = enable;
    if (m_gpuRenderer) {
        m_gpuRenderer->SetScanlines(enable);
    }
}

void CoreWrapper::SetBrightness(float brightness) {
    m_brightness = brightness;
    if (m_gpuRenderer) {
        m_gpuRenderer->SetBrightness(brightness);
    }
}

void CoreWrapper::SetGamma(float gamma) {
    m_gamma = gamma;
    if (m_gpuRenderer) {
        m_gpuRenderer->SetGamma(gamma);
    }
}

void CoreWrapper::SetFXAA(bool enable) {
    m_fxaa = enable;
    if (m_gpuRenderer) {
        m_gpuRenderer->SetFXAA(enable);
    }
}

void CoreWrapper::SetWireframeMode(bool enable) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    if (m_gpuRenderer) {
        m_gpuRenderer->SetWireframeMode(enable);
    }
}

const char* CoreWrapper::GetActiveRenderer() const {
    if (!m_initialized || !m_saturn) {
        return "Unknown";
    }
    
    return (m_gpuRenderer != nullptr) ? "Vulkan" : "Software";
}

bool CoreWrapper::IsGPURendererActive() const {
    if (!m_initialized || !m_saturn) {
        return false;
    }
    
    return (m_gpuRenderer != nullptr);
}

const char* CoreWrapper::GetLastRendererError() const {
    return m_lastRendererError.c_str();
}

void CoreWrapper::SetVerticalOverscan(bool enable) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    m_saturn->VDP.SetVerticalOverscan(enable);
}

void CoreWrapper::GetVisibleResolution(uint32_t& width, uint32_t& height) const {
    if (!m_initialized || !m_saturn) {
        width = 320;
        height = 224;
        return;
    }

    width = m_saturn->VDP.GetVisibleWidth();
    height = m_saturn->VDP.GetVisibleHeight();
}

void CoreWrapper::SetDeinterlacingMode(const char* mode) {
    if (!m_initialized || !m_saturn || !mode) {
        return;
    }

    using DeinterlaceMode = brimir::vdp::DeinterlaceMode;
    
    if (strcmp(mode, "blend") == 0) {
        m_saturn->VDP.SetDeinterlaceMode(DeinterlaceMode::Blend);
    } else if (strcmp(mode, "weave") == 0) {
        m_saturn->VDP.SetDeinterlaceMode(DeinterlaceMode::Weave);
    } else if (strcmp(mode, "bob") == 0) {
        m_saturn->VDP.SetDeinterlaceMode(DeinterlaceMode::Bob);
    } else if (strcmp(mode, "current") == 0) {
        m_saturn->VDP.SetDeinterlaceMode(DeinterlaceMode::Current);
    } else if (strcmp(mode, "none") == 0) {
        m_saturn->VDP.SetDeinterlaceMode(DeinterlaceMode::None);
    }
}

// TODO: Fix MSVC compilation errors
/*
bool CoreWrapper::LoadCartridgeRAM() {
    if (!m_hasCartridge || m_cartridgePath.empty()) {
        return false;
    }
    
    // Check if save file exists
    if (!std::filesystem::exists(m_cartridgePath)) {
        return false;  // No save file yet, that's OK
    }
    
    try {
        // Read the cartridge RAM file
        std::ifstream file(m_cartridgePath, std::ios::binary);
        if (!file) {
            return false;
        }
        
        // Get file size
        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        
        // Read data
        std::vector<uint8_t> data(fileSize);
        file.read(reinterpret_cast<char*>(data.data()), fileSize);
        
        // Get cartridge and load RAM based on type
        auto& cartSlot = m_saturn->GetCartridgeSlot();
        using CartType = brimir::cart::CartType;
        const auto cartType = cartSlot.GetCartridgeType();
        
        constexpr size_t size8 = 1024 * 1024;
        constexpr size_t size32 = 4 * 1024 * 1024;
        constexpr size_t size48 = 6 * 1024 * 1024;
        
        if (cartType == CartType::DRAM8Mbit && fileSize == size8) {
            auto* cart8 = static_cast<brimir::cart::DRAM8MbitCartridge*>(&cartSlot.GetCartridge());
            cart8->LoadRAM(std::span<const uint8_t, size8>(data.data(), size8));
            return true;
        } else if (cartType == CartType::DRAM32Mbit && fileSize == size32) {
            auto* cart32 = static_cast<brimir::cart::DRAM32MbitCartridge*>(&cartSlot.GetCartridge());
            cart32->LoadRAM(std::span<const uint8_t, size32>(data.data(), size32));
            return true;
        } else if (cartType == CartType::DRAM48Mbit && fileSize == size48) {
            auto* cart48 = static_cast<brimir::cart::DRAM48MbitCartridge*>(&cartSlot.GetCartridge());
            cart48->LoadRAM(std::span<const uint8_t, size48>(data.data(), size48));
            return true;
        }
        
        return false;
    } catch (...) {
        return false;
    }
}

void CoreWrapper::SaveCartridgeRAM() {
    if (!m_hasCartridge || m_cartridgePath.empty()) {
        return;
    }
    
    try {
        // Get cartridge and save RAM based on type
        auto& cartSlot = m_saturn->GetCartridgeSlot();
        using CartType = brimir::cart::CartType;
        const auto cartType = cartSlot.GetCartridgeType();
        
        std::vector<uint8_t> data;
        
        if (cartType == CartType::DRAM8Mbit) {
            constexpr size_t size8 = 1024 * 1024;
            data.resize(size8);
            auto* cart8 = static_cast<brimir::cart::DRAM8MbitCartridge*>(&cartSlot.GetCartridge());
            cart8->DumpRAM(std::span<uint8_t, size8>(data.data(), size8));
        } else if (cartType == CartType::DRAM32Mbit) {
            constexpr size_t size32 = 4 * 1024 * 1024;
            data.resize(size32);
            auto* cart32 = static_cast<brimir::cart::DRAM32MbitCartridge*>(&cartSlot.GetCartridge());
            cart32->DumpRAM(std::span<uint8_t, size32>(data.data(), size32));
        } else if (cartType == CartType::DRAM48Mbit) {
            constexpr size_t size48 = 6 * 1024 * 1024;
            data.resize(size48);
            auto* cart48 = static_cast<brimir::cart::DRAM48MbitCartridge*>(&cartSlot.GetCartridge());
            cart48->DumpRAM(std::span<uint8_t, size48>(data.data(), size48));
        } else {
            return;
        }
        
        // Ensure parent directory exists
        std::filesystem::create_directories(m_cartridgePath.parent_path());
        
        // Write to file
        std::ofstream file(m_cartridgePath, std::ios::binary);
        if (file) {
            file.write(reinterpret_cast<const char*>(data.data()), data.size());
        }
    } catch (...) {
        // Ignore errors - save will be retried next time
    }
}


*/

} // namespace brimir

