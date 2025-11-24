// Brimir - Core wrapper for Ymir emulator
// Copyright (C) 2025 Brimir Team
// Licensed under GPL-3.0

#include "brimir/core_wrapper.hpp"

#include <ymir/ymir.hpp>
#include <ymir/media/loader/loader.hpp>

#include <cstring>
#include <filesystem>

namespace brimir {

CoreWrapper::CoreWrapper() {
    // Reserve framebuffer space (max Saturn resolution)
    m_framebuffer.resize(704 * 512);
    
    // Reserve audio buffer space (enough for one frame at 44.1kHz)
    m_audioBuffer.resize(2048 * 2); // stereo
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
        
        m_initialized = true;
        return true;
    } catch (const std::exception& e) {
        // Log error
        m_saturn.reset();
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

bool CoreWrapper::LoadGame(const char* path) {
    if (!m_initialized || !m_saturn) {
        return false;
    }

    try {
        std::filesystem::path gamePath(path);
        
        // Create a Disc object and load the disc image into it
        ymir::media::Disc disc;
        
        // Callback for loader messages (can log these via libretro later)
        auto loaderCallback = [](ymir::media::MessageType type, std::string message) {
            // TODO: Forward to libretro logging
        };
        
        // Use Ymir's loader to load the disc
        bool success = ymir::media::LoadDisc(gamePath, disc, false, loaderCallback);
        
        if (!success || disc.sessions.empty()) {
            return false;
        }

        // Load the disc into the emulator
        m_saturn->LoadDisc(std::move(disc));
        m_saturn->CloseTray();
        
        // Auto-detect region from disc
        const auto& loadedDisc = m_saturn->GetDisc();
        if (!loadedDisc.sessions.empty()) {
            m_saturn->AutodetectRegion(loadedDisc.header.compatAreaCode);
        }
        
        m_gameLoaded = true;
        return true;
        
    } catch (const std::exception&) {
        return false;
    }
}

void CoreWrapper::UnloadGame() {
    if (!m_initialized || !m_saturn) {
        return;
    }

    m_saturn->EjectDisc();
    m_gameLoaded = false;
}

void CoreWrapper::RunFrame() {
    if (!m_initialized || !m_saturn) {
        return;
    }

    // Run one frame of emulation
    m_saturn->RunFrame();
    
    // TODO: Copy framebuffer from VDP
    // TODO: Copy audio samples from SCSP
}

void CoreWrapper::Reset() {
    if (!m_initialized || !m_saturn) {
        return;
    }

    m_saturn->Reset(false); // Soft reset
}

bool CoreWrapper::LoadIPL(const void* data, size_t size) {
    if (!m_initialized || !m_saturn) {
        return false;
    }

    // Ymir expects exactly 512KB for IPL
    constexpr size_t IPL_SIZE = 512 * 1024;
    if (size != IPL_SIZE) {
        return false;
    }

    try {
        // Create a span from the data
        auto iplSpan = std::span<uint8_t, IPL_SIZE>(
            static_cast<uint8_t*>(const_cast<void*>(data)), 
            IPL_SIZE
        );
        
        m_saturn->LoadIPL(iplSpan);
        return true;
        
    } catch (const std::exception& e) {
        return false;
    }
}

const void* CoreWrapper::GetFramebuffer() const {
    // For now, return our internal framebuffer
    // TODO: Get actual framebuffer from Ymir's VDP
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

size_t CoreWrapper::GetAudioSamples(int16_t* buffer, size_t max_samples) {
    if (!m_initialized || !m_saturn) {
        return 0;
    }

    // TODO: Get actual audio samples from Ymir's SCSP
    // For now, return silence
    std::memset(buffer, 0, max_samples * 2 * sizeof(int16_t));
    return max_samples;
}

size_t CoreWrapper::GetStateSize() const {
    if (!m_initialized || !m_saturn) {
        return 0;
    }

    // TODO: Determine actual state size from Ymir
    // For now, return a placeholder size
    return 16 * 1024 * 1024; // 16 MB placeholder
}

bool CoreWrapper::SaveState(void* data, size_t size) {
    if (!m_initialized || !m_saturn) {
        return false;
    }

    try {
        // TODO: Use Ymir's SaveState functionality
        // ymir::state::State state;
        // m_saturn->SaveState(state);
        // Serialize state to data buffer
        return false; // Not implemented yet
        
    } catch (const std::exception& e) {
        return false;
    }
}

bool CoreWrapper::LoadState(const void* data, size_t size) {
    if (!m_initialized || !m_saturn) {
        return false;
    }

    try {
        // TODO: Use Ymir's LoadState functionality
        // Deserialize state from data buffer
        // m_saturn->LoadState(state);
        return false; // Not implemented yet
        
    } catch (const std::exception& e) {
        return false;
    }
}

} // namespace brimir

