// Brimir - Core wrapper for Ymir emulator
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "brimir/core_wrapper.hpp"

#include <ymir/ymir.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/core/configuration.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_state_common.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_report.hpp>
#include <ymir/savestate/savestate.hpp>
#include <ymir/db/game_db.hpp>
#include <ymir/db/rom_cart_db.hpp>
#include <ymir/db/ipl_db.hpp>
#include <ymir/hw/cart/cart_impl_dram.hpp>
#include <ymir/core/hash.hpp>
#include <ymir/hw/smpc/smpc_defs.hpp>
#include <ymir/util/bit_ops.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>

#include <lz4.h>
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>

// SIMD intrinsics
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>  // SSE2
#if defined(__AVX2__)
#include <immintrin.h>  // AVX2 (superset of SSE)
#endif
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

namespace {

constexpr size_t kSaturnInternalBackupRAMSize = 32 * 1024;

constexpr uint8_t kPersistentSMPCDataVersion = 0x01;

constexpr uint32_t kNewStateMagic    = 0x32524942; // "BRI2" in LE
constexpr uint32_t kLegacyStateMagic = 0x4D495242; // "BRIM" in LE
constexpr uint32_t kSaveStateVersion = 1;

bool LoadPersistentSMPCDataFromFile(ymir::smpc::PersistentSMPCData &data,
                                    const std::filesystem::path &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return false;
    }

    const int version = in.get();
    if (version < 0 || static_cast<uint8_t>(version) != kPersistentSMPCDataVersion) {
        return false;
    }
    in.seekg(3, std::ios::cur); // skip 3 reserved bytes

    std::array<uint8_t, 4> smem{};
    uint8_t steRaw = 0;
    uint64_t rtcOffset = 0;
    uint64_t rtcTimestamp = 0;

    in.read(reinterpret_cast<char *>(smem.data()), smem.size());
    in.read(reinterpret_cast<char *>(&steRaw), sizeof(steRaw));
    in.read(reinterpret_cast<char *>(&rtcOffset), sizeof(rtcOffset));
    in.read(reinterpret_cast<char *>(&rtcTimestamp), sizeof(rtcTimestamp));
    if (!in) {
        return false;
    }

    data.SMEM = smem;
    data.STE = (steRaw != 0);
    data.rtc.offset = static_cast<sint64>(bit::little_endian_swap(rtcOffset));
    data.rtc.timestamp = static_cast<sint64>(bit::little_endian_swap(rtcTimestamp));
    return true;
}

void SavePersistentSMPCDataToFile(const ymir::smpc::PersistentSMPCData &data,
                                  const std::filesystem::path &path) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return;
    }

    out.put(static_cast<char>(kPersistentSMPCDataVersion));
    out.put(0x00); // reserved for future expansion
    out.put(0x00); // reserved for future expansion
    out.put(0x00); // reserved for future expansion

    const uint64_t rtcOffset = bit::little_endian_swap(static_cast<uint64_t>(data.rtc.offset));
    const uint64_t rtcTimestamp = bit::little_endian_swap(static_cast<uint64_t>(data.rtc.timestamp));

    out.write(reinterpret_cast<const char *>(data.SMEM.data()), data.SMEM.size());
    const uint8_t steRaw = data.STE ? uint8_t{1} : uint8_t{0};
    out.write(reinterpret_cast<const char *>(&steRaw), sizeof(steRaw));
    out.write(reinterpret_cast<const char *>(&rtcOffset), sizeof(rtcOffset));
    out.write(reinterpret_cast<const char *>(&rtcTimestamp), sizeof(rtcTimestamp));
}

} // namespace

namespace brimir {

CoreWrapper::CoreWrapper() {
    // Reserve framebuffer space (max Saturn resolution)
    m_framebuffer.resize(704 * 512);
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
        
        // Configure Ymir with optimized settings for libretro
        // Threaded VDP1/VDP2 provides better frame pacing and async rendering
        m_saturn->configuration.video.threadedVDP1 = true;
        m_saturn->configuration.video.threadedVDP2 = true;
        m_saturn->configuration.video.threadedDeinterlacer = true;
        
        // Configure enhancements for optimal libretro performance
        m_saturn->VDP.ModifyEnhancements([](ymir::vdp::config::Enhancements& enh) {
            enh.deinterlace = true;        // Bob mode deinterlacing
            enh.transparentMeshes = true;  // Accurate VDP1 mesh rendering
        });
        
        // Set up VDP software render callback to capture framebuffer
        m_saturn->VDP.SetSoftwareRenderCallback(
            {this, [](uint32* fb, uint32 width, uint32 height, void* ctx) {
                static_cast<CoreWrapper*>(ctx)->OnFrameComplete(fb, width, height);
            }});
        
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
        m_saturn->configuration.video.threadedVDP1 = true;
        m_saturn->configuration.video.threadedVDP2 = true;
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
        
        // Store game path and directories for later disc swaps
        m_gamePath = gamePath;
        m_saveDirectory = save_directory ? save_directory : "";
        m_systemDirectory = system_directory ? system_directory : "";

        // Handle M3U playlists for multi-disc games
        std::filesystem::path discToLoad = gamePath;
        std::string ext = gamePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (ext == ".m3u" || ext == ".m3u8") {
            // Parse M3U file
            m_discList.clear();
            std::ifstream m3uFile(gamePath);
            if (!m3uFile) {
                m_lastError = "Failed to open M3U file: " + gamePath.string();
                m_initialDiscSet = false;
                return false;
            }
            std::string line;
            std::filesystem::path m3uDir = gamePath.parent_path();
            while (std::getline(m3uFile, line)) {
                // Trim whitespace
                size_t start = 0;
                while (start < line.size() && (line[start] == ' ' || line[start] == '\t' || line[start] == '\r')) start++;
                size_t end = line.size();
                while (end > start && (line[end-1] == ' ' || line[end-1] == '\t' || line[end-1] == '\r')) end--;
                if (start >= end || line[start] == '#') continue;
                std::string entry = line.substr(start, end - start);
                std::filesystem::path entryPath(entry);
                if (entryPath.is_relative()) {
                    entryPath = m3uDir / entryPath;
                }
                m_discList.push_back(std::filesystem::absolute(entryPath));
            }
            if (m_discList.empty()) {
                m_lastError = "M3U file contains no valid disc entries";
                m_initialDiscSet = false;
                return false;
            }
            // Determine which disc to load
            size_t loadIndex = 0;
            if (m_initialDiscSet && m_initialDiscIndex < m_discList.size()) {
                loadIndex = m_initialDiscIndex;
            }
            m_currentDiscIndex = loadIndex;
            discToLoad = m_discList[loadIndex];
        } else {
            // Single disc: create a one-entry disc list for consistency
            m_discList.clear();
            m_discList.push_back(std::filesystem::absolute(gamePath));
            m_currentDiscIndex = 0;
        }
        m_initialDiscSet = false;

        // Set up persistent backup RAM path (Ymir uses memory-mapped files)
        // Use save_directory + game name to create a persistent backup RAM file
        // For M3U games, use the M3U filename (all discs share the same backup RAM)
        std::filesystem::path gameFileName = gamePath.stem();
        if (save_directory && save_directory[0] != '\0') {
            m_sramTempPath = std::filesystem::path(save_directory) / (gameFileName.string() + ".bup");
        } else {
            // Fallback to temp directory if no save_directory provided
            m_sramTempPath = std::filesystem::temp_directory_path() / (gameFileName.string() + ".bup");
        }
        
        // Ensure parent directory exists
        std::filesystem::create_directories(m_sramTempPath.parent_path());
        
        // Load backup RAM from persistent file (creates if doesn't exist)
        // Use copyOnWrite=true to allow modifications without affecting the file on disk
        std::error_code error;
        m_saturn->LoadInternalBackupMemoryImage(m_sramTempPath, true, error);
        if (error) {
            m_lastError = "Failed to load backup RAM from " + m_sramTempPath.string() + ": " + error.message();
            // Don't fail game load - just continue with fresh backup RAM
        }
        
        // Load SMPC persistent data (RTC clock settings!)
        // This is system-wide (not per-game) as the RTC is a console setting, not a game setting.
        // The filename is qualified by the loaded IPL ROM region to keep per-console settings separate.
        if (system_directory && system_directory[0] != '\0') {
            m_smpcBaseDir = std::filesystem::path(system_directory);
        } else if (save_directory && save_directory[0] != '\0') {
            // Fallback to save directory if no system_directory provided
            m_smpcBaseDir = std::filesystem::path(save_directory);
        }
        std::filesystem::create_directories(m_smpcBaseDir);

        // Register SMPC data persistence callback so core settings are saved whenever they change.
        m_saturn->SMPC.SetPersistDataCallback({this, &CoreWrapper::OnPersistSMPCData});

        // Load existing SMPC data for the current BIOS region.
        ymir::smpc::PersistentSMPCData smpcData{};
        const auto smpcPath = GetPersistentSMPCDataPath();
        if (!smpcPath.empty()) {
            if (!LoadPersistentSMPCDataFromFile(smpcData, smpcPath)) {
                // Migrate from the old non-region-qualified filename, if present.
                const auto legacyPath = m_smpcBaseDir / "brimir_saturn_rtc.smpc";
                if (LoadPersistentSMPCDataFromFile(smpcData, legacyPath)) {
                    std::error_code migrateError;
                    std::filesystem::create_directories(smpcPath.parent_path(), migrateError);
                    SavePersistentSMPCDataToFile(smpcData, smpcPath);
                }
            }
            m_saturn->SMPC.LoadPersistentData(smpcData);
        }
        
        // Bring Ymir's backup RAM into the canonical SRAM buffer. If the frontend
        // already provided data (e.g. an .srm loaded before retro_load_game), keep
        // that buffer authoritative and copy it into Ymir instead.
        if (m_sramData.empty()) {
            m_sramData = m_saturn->mem.GetInternalBackupRAM().ReadAll();
        } else {
            WriteSRAMToYmir();
        }
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
            success = ymir::media::LoadDisc(discToLoad, disc, false, loaderCallback);
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
            const ymir::db::GameInfo* gameInfo = ymir::db::GetGameInfo(
                discForCartridge.header.productNumber,
                m_saturn->GetDiscHash()
            );
            
                if (gameInfo && gameInfo->GetCartridge() != ymir::db::Cartridge::None) {
                // Set up cartridge RAM save path
                if (save_directory && save_directory[0] != '\0') {
                    m_cartridgePath = std::filesystem::path(save_directory) / (gamePath.stem().string() + ".cart");
                } else {
                    m_cartridgePath = std::filesystem::temp_directory_path() / (gamePath.stem().string() + ".cart");
                }
                
                // Insert the appropriate cartridge
                try {
                    switch (gameInfo->GetCartridge()) {
                    case ymir::db::Cartridge::DRAM8Mbit:
                        m_saturn->InsertCartridge<ymir::cart::DRAM8MbitCartridge>();
                        m_hasCartridge = true;
                        break;
                    case ymir::db::Cartridge::DRAM32Mbit:
                        m_saturn->InsertCartridge<ymir::cart::DRAM32MbitCartridge>();
                        m_hasCartridge = true;
                        break;
                    case ymir::db::Cartridge::DRAM48Mbit:
                        m_saturn->InsertCartridge<ymir::cart::DRAM48MbitCartridge>();
                        m_hasCartridge = true;
                        break;
                    default:
                        // Other cartridge types (ROM, BackupRAM) not yet supported
                        break;
                    }
                    
                    // Do a HARD reset after inserting cartridge to force BIOS reboot
                    // This is necessary because the BIOS has already initialized
                    m_saturn->Reset(true);

                    // Load saved cartridge RAM AFTER reset (Reset zeros the RAM)
                    LoadCartridgeRAM();
                    
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
        m_saturn->configuration.video.threadedVDP1 = true;
        m_saturn->configuration.video.threadedVDP2 = true;
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

void CoreWrapper::SaveCartridgeRAM() {
    if (!m_saturn || !m_hasCartridge || m_cartridgePath.empty()) {
        return;
    }

    auto& cart = m_saturn->GetCartridge();
    const auto type = cart.GetType();

    std::vector<uint8> ram;

    switch (type) {
    case ymir::cart::CartType::DRAM8Mbit: {
        auto* dram = cart.As<ymir::cart::CartType::DRAM8Mbit>();
        if (!dram) return;
        ram.resize(1_MiB);
        dram->DumpRAM(std::span<uint8, 1_MiB>(ram.data(), ram.size()));
        break;
    }
    case ymir::cart::CartType::DRAM32Mbit: {
        auto* dram = cart.As<ymir::cart::CartType::DRAM32Mbit>();
        if (!dram) return;
        ram.resize(4_MiB);
        dram->DumpRAM(std::span<uint8, 4_MiB>(ram.data(), ram.size()));
        break;
    }
    case ymir::cart::CartType::DRAM48Mbit: {
        auto* dram = cart.As<ymir::cart::CartType::DRAM48Mbit>();
        if (!dram) return;
        ram.resize(6_MiB);
        dram->DumpRAM(std::span<uint8, 6_MiB>(ram.data(), ram.size()));
        break;
    }
    default:
        return;
    }

    try {
        std::ofstream file(m_cartridgePath, std::ios::binary | std::ios::trunc);
        if (file.is_open()) {
            file.write(reinterpret_cast<const char*>(ram.data()), ram.size());
        }
    } catch (...) {
        m_lastError = "Failed to save cartridge RAM";
    }
}

void CoreWrapper::LoadCartridgeRAM() {
    if (!m_saturn || !m_hasCartridge || m_cartridgePath.empty()) {
        return;
    }

    if (!std::filesystem::exists(m_cartridgePath)) {
        return;
    }

    auto& cart = m_saturn->GetCartridge();
    const auto type = cart.GetType();

    std::vector<uint8> ram;
    size_t expectedSize = 0;

    switch (type) {
    case ymir::cart::CartType::DRAM8Mbit:
        expectedSize = 1_MiB;
        break;
    case ymir::cart::CartType::DRAM32Mbit:
        expectedSize = 4_MiB;
        break;
    case ymir::cart::CartType::DRAM48Mbit:
        expectedSize = 6_MiB;
        break;
    default:
        return;
    }

    try {
        std::ifstream file(m_cartridgePath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return;

        const auto fileSize = static_cast<size_t>(file.tellg());
        if (fileSize != expectedSize) return;

        file.seekg(0, std::ios::beg);
        ram.resize(expectedSize);
        file.read(reinterpret_cast<char*>(ram.data()), expectedSize);
    } catch (...) {
        return;
    }

    switch (type) {
    case ymir::cart::CartType::DRAM8Mbit: {
        auto* dram = cart.As<ymir::cart::CartType::DRAM8Mbit>();
        if (dram) dram->LoadRAM(std::span<const uint8, 1_MiB>(ram.data(), 1_MiB));
        break;
    }
    case ymir::cart::CartType::DRAM32Mbit: {
        auto* dram = cart.As<ymir::cart::CartType::DRAM32Mbit>();
        if (dram) dram->LoadRAM(std::span<const uint8, 4_MiB>(ram.data(), 4_MiB));
        break;
    }
    case ymir::cart::CartType::DRAM48Mbit: {
        auto* dram = cart.As<ymir::cart::CartType::DRAM48Mbit>();
        if (dram) dram->LoadRAM(std::span<const uint8, 6_MiB>(ram.data(), 6_MiB));
        break;
    }
    default:
        break;
    }
}

void CoreWrapper::UnloadGame() {
    if (!m_initialized || !m_saturn) {
        return;
    }
    
    // CRITICAL: Stop threaded VDP FIRST to prevent race conditions
    // This gracefully shuts down the render thread before we do any cleanup
    bool wasThreadedVDP1 = m_saturn->configuration.video.threadedVDP1.Get();
    bool wasThreadedVDP2 = m_saturn->configuration.video.threadedVDP2.Get();
    if (wasThreadedVDP1 || wasThreadedVDP2) {
        m_saturn->configuration.video.threadedVDP1 = false;
        m_saturn->configuration.video.threadedVDP2 = false;
        // Give the thread time to shut down cleanly
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // Save cartridge RAM if present
    if (m_hasCartridge) {
        SaveCartridgeRAM();
    }
    
    // Save SMPC persistent data (RTC clock!) before unloading
    // This preserves the date/time the user set (system-wide, not per-game)
    if (m_saturn) {
        try {
            m_saturn->SMPC.PersistData();
        } catch (...) {
            // Non-critical, continue
        }
    }
    
    // Read final SRAM state from Ymir BEFORE unloading
    // This ensures RetroArch can save the latest state (including clock settings)
    if (m_sramInitialized) {
        m_sramData = m_saturn->mem.GetInternalBackupRAM().ReadAll();
    }

    // Clear the in-memory SRAM buffer so it cannot leak into the next game.
    // The frontend is expected to save the buffer before calling unload.
    m_sramData.clear();

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

std::string CoreWrapper::GetSMPCRegionSuffix() const {
    if (!m_saturn) {
        return "none";
    }

    const auto *info = ymir::db::GetIPLROMInfo(m_saturn->GetIPLHash());
    if (!info) {
        return "none";
    }

    switch (info->region) {
    case ymir::db::SystemRegion::US_EU: return "us_eu";
    case ymir::db::SystemRegion::JP: return "jp";
    case ymir::db::SystemRegion::KR: return "asia";
    default: return "other";
    }
}

std::filesystem::path CoreWrapper::GetPersistentSMPCDataPath() const {
    if (m_smpcBaseDir.empty()) {
        return {};
    }
    return m_smpcBaseDir / ("brimir_saturn_rtc_" + GetSMPCRegionSuffix() + ".smpc");
}

void CoreWrapper::OnPersistSMPCData(const ymir::smpc::PersistentSMPCData &data, void *ctx) {
    auto *self = static_cast<CoreWrapper *>(ctx);
    if (!self) {
        return;
    }
    const auto path = self->GetPersistentSMPCDataPath();
    if (path.empty()) {
        return;
    }
    std::error_code dirError;
    std::filesystem::create_directories(path.parent_path(), dirError);
    SavePersistentSMPCDataToFile(data, path);
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

    // On the first call after RetroArch has loaded .srm data,
    // write it back into Ymir's backup RAM so the emulator uses it
    if (m_gameLoaded && m_sramFirstLoad) {
        WriteSRAMToYmir();
        m_sramFirstLoad = false;
        m_sramCacheDirty = false;
        m_framesSinceLastSRAMSync = 0;
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

    // Saturn internal backup RAM is 32 KiB. Until LoadGame() has finished,
    // Ymir's backup RAM may not be safely queried, so return the fixed size.
    if (!m_gameLoaded) {
        return kSaturnInternalBackupRAMSize;
    }

    return m_saturn->mem.GetInternalBackupRAM().Size();
}

ConsoleRegion CoreWrapper::GetConsoleRegion() const {
    if (!m_initialized || !m_saturn) {
        return ConsoleRegion::Unknown;
    }

    switch (m_saturn->SMPC.GetAreaCode()) {
        case 1:  // Japan
        case 2:  // Asia NTSC
        case 4:  // North America
        case 5:  // Central/South America NTSC
        case 6:  // Korea
            return ConsoleRegion::NTSC;
        case 12: // Europe PAL
        case 10: // Asia PAL
        case 13: // Central/South America PAL
            return ConsoleRegion::PAL;
        default:
            return ConsoleRegion::NTSC;
    }
}

bool CoreWrapper::SetSRAMData(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return false;
    }
    if (size != GetSRAMSize()) {
        return false;
    }

    m_sramData.assign(data, data + size);

    if (m_gameLoaded && m_saturn) {
        WriteSRAMToYmir();
        m_sramCacheDirty = false;
        m_framesSinceLastSRAMSync = 0;
        m_sramFirstLoad = false;
    }
    return true;
}

void CoreWrapper::WriteSRAMToYmir() const {
    if (!m_initialized || !m_saturn || m_sramData.empty()) {
        return;
    }

    auto& bup = m_saturn->mem.GetInternalBackupRAM();
    const uint32_t size = bup.Size();
    for (uint32_t i = 0; i < size && i < m_sramData.size(); ++i) {
        bup.WriteByte(i * 2, m_sramData[i]);
    }
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

void* CoreWrapper::GetSystemRAMData() {
    if (!m_initialized || !m_saturn) {
        return nullptr;
    }
    return m_saturn->mem.WRAMLow.data();
}

size_t CoreWrapper::GetSystemRAMSize() const {
    if (!m_initialized || !m_saturn) {
        return 0;
    }
    return m_saturn->mem.WRAMLow.size();
}

void* CoreWrapper::GetSRAMRawPointer() {
    if (!m_initialized || !m_saturn) {
        return nullptr;
    }
    if (m_sramData.empty()) {
        m_sramData.resize(GetSRAMSize());
    }
    return m_sramData.data();
}

void* CoreWrapper::GetSystemRAMRawPointer() {
    if (!m_initialized || !m_saturn) {
        return nullptr;
    }
    return m_saturn->mem.WRAMLow.data();
}

void* CoreWrapper::GetSystemRAMHighRawPointer() {
    if (!m_initialized || !m_saturn) {
        return nullptr;
    }
    return m_saturn->mem.WRAMHigh.data();
}

size_t CoreWrapper::GetSystemRAMHighSize() const {
    if (!m_initialized || !m_saturn) {
        return 0;
    }
    return m_saturn->mem.WRAMHigh.size();
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
    return m_displayPointer ? m_displayPointer : m_framebuffer.data();
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


size_t CoreWrapper::GetAudioSamples(int16_t* buffer, size_t max_samples) {
    if (!m_initialized || !m_saturn || !buffer) {
        return 0;
    }
    // Clamp to a sensible per-frame upper bound to avoid huge copies
    if (max_samples > kAudioRingBufferStereoCapacity) {
        max_samples = kAudioRingBufferStereoCapacity;
    }
    return m_audioRingBuffer.Pop(buffer, max_samples);
}

size_t CoreWrapper::GetStateSize() const {
    if (!m_initialized || !m_saturn) {
        return 0;
    }

    // Header (12 bytes) + max compressed size.
    // NOTE: The save-state header currently does not store the compressed
    // payload size; LoadState() assumes all bytes after the header are LZ4
    // data. Adding a compressedSize field would break the existing save-state
    // header contract, so this is left as a known follow-up for a future
    // compatibility bump. Existing save-state tests rely on the current layout.
    return static_cast<size_t>(LZ4_compressBound(static_cast<int>(sizeof(ymir::savestate::SaveState)))) + 12;
}

bool CoreWrapper::SaveState(void* data, size_t size) {
    if (!m_initialized || !m_saturn || !data) {
        return false;
    }
    
    // Verify buffer is large enough for header + max compressed data
    size_t requiredSize = GetStateSize();
    if (size < requiredSize) {
        return false;
    }

    try {
        // Create a State object on the heap (too large for stack)
        auto state = std::make_unique<ymir::savestate::SaveState>();
        m_saturn->SaveState(*state);
        
        // Write 12-byte header: magic + version + uncompressed size
        const uint32_t magic = kNewStateMagic;
        const uint32_t version = kSaveStateVersion;
        const uint32_t uncompSize = static_cast<uint32_t>(sizeof(ymir::savestate::SaveState));
        auto* out = static_cast<uint8_t*>(data);
        std::memcpy(out, &magic, 4);
        std::memcpy(out + 4, &version, 4);
        std::memcpy(out + 8, &uncompSize, 4);

        // Compress with LZ4
        int srcSize = static_cast<int>(sizeof(ymir::savestate::SaveState));
        int dstCapacity = static_cast<int>(size - 12);
        int compressedSize = LZ4_compress_default(
            reinterpret_cast<const char*>(state.get()),
            reinterpret_cast<char*>(out + 12),
            srcSize,
            dstCapacity
        );
        
        if (compressedSize <= 0) {
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        (void)e;
        return false;
    }
}

bool CoreWrapper::LoadState(const void* data, size_t size) {
    if (!m_initialized || !m_saturn || !data) {
        return false;
    }

    try {
        const auto* in = static_cast<const uint8_t*>(data);

        if (size < 8) {
            return false;
        }

        uint32_t magic = 0;
        std::memcpy(&magic, in, 4);

        // New format: [magic:4][version:4][uncompSize:4] followed by LZ4 data
        if (magic == kNewStateMagic && size >= 12) {
            uint32_t version = 0;
            uint32_t uncompSize = 0;
            std::memcpy(&version, in + 4, 4);
            std::memcpy(&uncompSize, in + 8, 4);

            if (version != kSaveStateVersion) {
                return false;
            }
            if (uncompSize != sizeof(ymir::savestate::SaveState)) {
                return false;
            }

            auto state = std::make_unique<ymir::savestate::SaveState>();
            int compressedSize = static_cast<int>(size - 12);
            int result = LZ4_decompress_safe(
                reinterpret_cast<const char*>(in + 12),
                reinterpret_cast<char*>(state.get()),
                compressedSize,
                static_cast<int>(uncompSize)
            );
            if (result != static_cast<int>(uncompSize)) {
                return false;
            }
            return m_saturn->LoadState(*state, true);
        }

        // Old Brimir compressed format: [magic:4][uncompSize:4] followed by LZ4 data
        if (magic == kLegacyStateMagic) {
            uint32_t uncompSize = 0;
            std::memcpy(&uncompSize, in + 4, 4);
            if (uncompSize != sizeof(ymir::savestate::SaveState)) {
                return false;
            }

            auto state = std::make_unique<ymir::savestate::SaveState>();
            int compressedSize = static_cast<int>(size - 8);
            int result = LZ4_decompress_safe(
                reinterpret_cast<const char*>(in + 8),
                reinterpret_cast<char*>(state.get()),
                compressedSize,
                static_cast<int>(uncompSize)
            );
            if (result != static_cast<int>(uncompSize)) {
                return false;
            }
            return m_saturn->LoadState(*state, true);
        }

        // Raw uncompressed state (fallback)
        size_t requiredSize = sizeof(ymir::savestate::SaveState);
        if (size < requiredSize) {
            return false;
        }
        auto state = std::make_unique<ymir::savestate::SaveState>();
        std::memcpy(state.get(), data, requiredSize);
        return m_saturn->LoadState(*state, true);
    } catch (const std::exception& e) {
        (void)e;
        return false;
    }
}


void CoreWrapper::OnFrameComplete(uint32_t* fb, uint32_t width, uint32_t height) {
    ScopedTimer timer(m_profiler, "OnFrameComplete_Total");
    
    if (!fb || width == 0 || height == 0) {
        return;
    }

    // Ymir's software renderer provides the full framebuffer at native resolution
    // No overscan cropping is applied by the VDP - we output the full frame
    m_fbWidth = width;
    m_fbHeight = height;
    m_fbPitch = width * 4; // XRGB8888 = 4 bytes per pixel

    // Resize framebuffer if needed
    size_t pixelCount = static_cast<size_t>(width) * height;
    if (m_framebuffer.size() < pixelCount) {
        m_framebuffer.resize(pixelCount);
    }

    // Convert from Ymir's XBGR8888 (0x00BBGGRR) to libretro XRGB8888 (0x00RRGGBB)
    // This is the exact same composition as Ymir outputs - no modifications
    {
        ScopedTimer convTimer(m_profiler, "PixelConversion");
        
        uint32_t* dst = m_framebuffer.data();
        
        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t* srcLine = fb + (y * width);
            uint32_t* dstLine = dst + (y * width);
            
            size_t i = 0;
            
#if defined(_M_X64) || defined(__x86_64__)
    #if defined(__AVX2__)
            // AVX2: Process 8 pixels at a time (256-bit registers)
            {
                const __m256i maskRB256 = _mm256_set1_epi32(0x00FF00FF);
                const __m256i maskG256  = _mm256_set1_epi32(0x0000FF00);
                
                for (; i + 8 <= width; i += 8) {
                    __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&srcLine[i]));
                    __m256i rb = _mm256_and_si256(pixels, maskRB256);
                    __m256i rb_swapped = _mm256_or_si256(_mm256_slli_epi32(rb, 16), _mm256_srli_epi32(rb, 16));
                    rb_swapped = _mm256_and_si256(rb_swapped, maskRB256);
                    __m256i g = _mm256_and_si256(pixels, maskG256);
                    __m256i result = _mm256_or_si256(rb_swapped, g);
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&dstLine[i]), result);
                }
            }
    #endif
    #if defined(__SSE2__) || defined(_M_X64)
            // SSE2: Process 4 pixels at a time (handles remaining after AVX2, or all if no AVX2)
            {
                const __m128i maskRB = _mm_set1_epi32(0x00FF00FF);
                const __m128i maskG  = _mm_set1_epi32(0x0000FF00);
                
                for (; i + 4 <= width; i += 4) {
                    __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&srcLine[i]));
                    __m128i rb = _mm_and_si128(pixels, maskRB);
                    __m128i rb_swapped = _mm_or_si128(_mm_slli_epi32(rb, 16), _mm_srli_epi32(rb, 16));
                    rb_swapped = _mm_and_si128(rb_swapped, maskRB);
                    __m128i g = _mm_and_si128(pixels, maskG);
                    __m128i result = _mm_or_si128(rb_swapped, g);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(&dstLine[i]), result);
                }
            }
    #endif
#endif
            
            // Scalar fallback for remaining pixels
            for (; i < width; ++i) {
                uint32_t pixel = srcLine[i];
                uint32_t r = pixel & 0x000000FF;
                uint32_t g = pixel & 0x0000FF00;
                uint32_t b = pixel & 0x00FF0000;
                dstLine[i] = (r << 16) | g | (b >> 16);
            }
        }
    }

    // Apply overscan crop and/or TATE rotation to produce final display buffer
    uint32_t srcWidth = width;
    uint32_t srcHeight = height;
    const uint32_t* srcBuf = m_framebuffer.data();

    // Step 1: Overscan crop
    if (m_overscanCropH > 0 || m_overscanCropV > 0) {
        uint32_t cropW = width;
        uint32_t cropH = height;

        if (static_cast<int>(width) > m_overscanCropH + 32 && static_cast<int>(height) > m_overscanCropV + 32) {
            cropW = width - static_cast<uint32_t>(m_overscanCropH);
            cropH = height - static_cast<uint32_t>(m_overscanCropV);
        }

        if (cropW != width || cropH != height) {
            int cropLeft = m_overscanCropH / 2;
            int cropTop = m_overscanCropV / 2;

            size_t cropPixels = static_cast<size_t>(cropW) * cropH;
            if (m_displayFramebuffer.size() < cropPixels) {
                m_displayFramebuffer.resize(cropPixels);
            }

            for (uint32_t y = 0; y < cropH; ++y) {
                const uint32_t* srcRow = m_framebuffer.data() + (static_cast<size_t>(y + cropTop) * width + cropLeft);
                uint32_t* dstRow = m_displayFramebuffer.data() + (static_cast<size_t>(y) * cropW);
                std::memcpy(dstRow, srcRow, cropW * sizeof(uint32_t));
            }

            srcBuf = m_displayFramebuffer.data();
            srcWidth = cropW;
            srcHeight = cropH;
        }
    }

    // Step 2: TATE rotation
    if (m_rotation != 0) {
        uint32_t rotW = (m_rotation == 90 || m_rotation == 270) ? srcHeight : srcWidth;
        uint32_t rotH = (m_rotation == 90 || m_rotation == 270) ? srcWidth : srcHeight;

        size_t rotPixels = static_cast<size_t>(rotW) * rotH;
        std::vector<uint32_t>* rotBuf = (srcBuf == m_displayFramebuffer.data())
            ? &m_framebuffer  // Reuse other buffer to avoid extra allocation
            : &m_displayFramebuffer;

        if (rotBuf->size() < rotPixels) {
            rotBuf->resize(rotPixels);
        }

        switch (m_rotation) {
            case 90:
                for (uint32_t y = 0; y < rotH; ++y) {
                    for (uint32_t x = 0; x < rotW; ++x) {
                        (*rotBuf)[static_cast<size_t>(y) * rotW + x] =
                            srcBuf[static_cast<size_t>(rotW - 1 - x) * srcWidth + y];
                    }
                }
                break;
            case 180:
                for (uint32_t y = 0; y < rotH; ++y) {
                    for (uint32_t x = 0; x < rotW; ++x) {
                        (*rotBuf)[static_cast<size_t>(y) * rotW + x] =
                            srcBuf[static_cast<size_t>(rotH - 1 - y) * rotW + (rotW - 1 - x)];
                    }
                }
                break;
            case 270:
                for (uint32_t y = 0; y < rotH; ++y) {
                    for (uint32_t x = 0; x < rotW; ++x) {
                        (*rotBuf)[static_cast<size_t>(y) * rotW + x] =
                            srcBuf[static_cast<size_t>(x) * srcWidth + (srcWidth - 1 - y)];
                    }
                }
                break;
        }

        m_displayPointer = rotBuf->data();
        m_fbWidth = rotW;
        m_fbHeight = rotH;
        m_fbPitch = rotW * 4;
    } else if (srcBuf != m_framebuffer.data()) {
        m_displayPointer = m_displayFramebuffer.data();
        m_fbWidth = srcWidth;
        m_fbHeight = srcHeight;
        m_fbPitch = srcWidth * 4;
    } else {
        m_displayPointer = nullptr;
    }
}


void CoreWrapper::OnAudioSample(int16_t left, int16_t right) {
    if (m_audioVolumeFixed != 65536) {
        left  = static_cast<int16_t>((static_cast<int64_t>(left)  * m_audioVolumeFixed) >> 16);
        right = static_cast<int16_t>((static_cast<int64_t>(right) * m_audioVolumeFixed) >> 16);
    }

    if (!m_audioRingBuffer.Push(left, right)) {
        // Optional: count overflows; keep branch cold
    }
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

void CoreWrapper::SetSH2OverclockFactor(uint32_t factor) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    if (factor < 100) factor = 100;
    if (factor > 300) factor = 300;

    m_saturn->SetSH2OverclockFactor(factor);
}

void CoreWrapper::SetAutodetectRegion(bool enable) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    m_saturn->configuration.system.autodetectRegion = enable;
}

void CoreWrapper::SetRenderer(const char* renderer) {
    if (!m_initialized || !m_saturn || !renderer) {
        return;
    }

    // Ymir only supports software rendering (Null and Software types)
    if (strcmp(renderer, "software") == 0) {
        // Software renderer is always active with Ymir hw layer
    }
}

void CoreWrapper::SetDeinterlacing(bool enable) {
    if (!m_initialized || !m_saturn) {
        return;
    }

    m_saturn->VDP.ModifyEnhancements([enable](ymir::vdp::config::Enhancements& enh) {
        enh.deinterlace = enable;
    });
}


const char* CoreWrapper::GetActiveRenderer() const {
    if (!m_initialized || !m_saturn) {
        return "Unknown";
    }
    
    return "Software";
}


void CoreWrapper::SetDeinterlacingMode(const char* mode) {
    if (!m_initialized || !m_saturn || !mode) {
        return;
    }

    // Ymir only supports enable/disable deinterlacing via Enhancements
    // The specific modes (blend/weave/bob/current) were Brimir enhancements
    // For now, just toggle deinterlacing on/off
    bool enable = (strcmp(mode, "none") != 0);
    m_saturn->VDP.ModifyEnhancements([enable](ymir::vdp::config::Enhancements& enh) {
        enh.deinterlace = enable;
    });
}

void CoreWrapper::SetAudioVolume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 200) percent = 200;
    m_audioVolume = percent;
    m_audioVolumeFixed = (percent * 65536) / 100;
}

void CoreWrapper::SetRotation(int degrees) {
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
        degrees = 0;
    }
    m_rotation = degrees;
}

void CoreWrapper::SetOverscanCrop(int horizontal, int vertical) {
    if (horizontal < 0) horizontal = 0;
    if (vertical < 0) vertical = 0;
    m_overscanCropH = horizontal;
    m_overscanCropV = vertical;
}

// --- Disk control (multi-disc support) ---

size_t CoreWrapper::GetNumDiscs() const {
    return m_discList.size();
}

size_t CoreWrapper::GetCurrentDiscIndex() const {
    return m_currentDiscIndex;
}

bool CoreWrapper::GetDiscPath(unsigned index, char* s, size_t len) const {
    if (index >= m_discList.size() || !s || len == 0) {
        return false;
    }
    std::string path = m_discList[index].string();
    if (path.size() >= len) {
        return false;
    }
    std::memcpy(s, path.c_str(), path.size() + 1);
    return true;
}

bool CoreWrapper::GetDiscLabel(unsigned index, char* s, size_t len) const {
    if (index >= m_discList.size() || !s || len == 0) {
        return false;
    }
    std::string label = m_discList[index].stem().string();
    if (label.size() >= len) {
        return false;
    }
    std::memcpy(s, label.c_str(), label.size() + 1);
    return true;
}

bool CoreWrapper::SetEjectState(bool ejected) {
    if (!m_initialized || !m_saturn || !m_gameLoaded) {
        return false;
    }
    if (ejected) {
        m_saturn->OpenTray();
    } else {
        m_saturn->CloseTray();
    }
    return true;
}

bool CoreWrapper::GetEjectState() const {
    if (!m_initialized || !m_saturn) {
        return false;
    }
    return m_saturn->IsTrayOpen();
}

bool CoreWrapper::SetDiscIndex(unsigned index) {
    if (!m_initialized || !m_saturn) {
        return false;
    }
    // Eject is equivalent to "remove disc" from drive
    if (index >= m_discList.size()) {
        m_saturn->EjectDisc();
        m_currentDiscIndex = m_discList.size();
        return true;
    }
    const auto& discPath = m_discList[index];
    if (!std::filesystem::exists(discPath)) {
        return false;
    }
    // Validate that we can parse the disc before ejecting the current one
    ymir::media::Disc disc;
    auto loaderCallback = [this](ymir::media::MessageType type, std::string message) {
        if (type == ymir::media::MessageType::Error) {
            if (!m_lastError.empty()) m_lastError += "; ";
            m_lastError += message;
        }
    };
    if (!ymir::media::LoadDisc(discPath, disc, false, loaderCallback)) {
        if (m_lastError.empty()) {
            m_lastError = "Failed to load disc: " + discPath.string();
        }
        return false;
    }
    // Now eject the current disc and insert the new one
    m_saturn->EjectDisc();
    m_saturn->LoadDisc(std::move(disc));
    m_currentDiscIndex = index;
    m_sramCacheDirty = true;
    return true;
}

bool CoreWrapper::AddDiscIndex() {
    m_discList.emplace_back();
    return true;
}

bool CoreWrapper::ReplaceDiscIndex(unsigned index, const char* path) {
    if (index >= m_discList.size()) {
        return false;
    }
    if (!path || path[0] == '\0') {
        // Removal: shift everything down
        m_discList.erase(m_discList.begin() + index);
        if (m_currentDiscIndex > index) {
            m_currentDiscIndex--;
        } else if (m_currentDiscIndex == index) {
            // Currently loaded disc was removed from playlist
            m_currentDiscIndex = m_discList.size();
        }
        return true;
    }
    m_discList[index] = std::filesystem::path(path);
    return true;
}

bool CoreWrapper::SetInitialDisc(unsigned index, const char* path) {
    if (!path) {
        m_initialDiscSet = false;
        return false;
    }
    // Store initial disc info; validation happens in LoadGame when M3U is parsed
    m_initialDiscSet = true;
    m_initialDiscIndex = index;
    m_initialDiscPath = std::filesystem::path(path);
    return true;
}


} // namespace brimir