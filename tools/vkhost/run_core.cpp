// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "host.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace vkhost {

namespace {

namespace fs = std::filesystem;

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

// The BIOS file names the core looks for, with their brimir_bios option values (src/libretro/libretro.cpp).
const char* BiosOptionForFile(const std::string& fileName) {
    static const struct {
        const char* file;
        const char* option;
    } known[] = {
        {"sega saturn bios v1.01 (jap).bin", "jp_101"}, {"sega saturn bios v1.00 (jap).bin", "jp_100"},
        {"sega_101.bin", "us_101"},                     {"mpr-17933.bin", "us_100"},
        {"sega_100.bin", "eu_100"},                     {"sega saturn bios (eur).bin", "eu_alt"},
    };
    const std::string lower = Lower(fileName);
    for (const auto& entry : known) {
        if (lower == entry.file) {
            return entry.option;
        }
    }
    return nullptr;
}

// Fresh directory under the temp directory, removed on destruction.
class ScratchDir {
public:
    ScratchDir() {
#ifdef _WIN32
        const unsigned long pid = GetCurrentProcessId();
#else
        const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() / "brimir_vkhost" /
                 ("run-" + std::to_string(pid) + "-" + std::to_string(static_cast<long long>(ticks)));
        std::error_code ec;
        fs::create_directories(m_path, ec);
        if (ec) {
            throw std::runtime_error("cannot create " + m_path.string() + ": " + ec.message());
        }
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    const fs::path& Path() const { return m_path; }

private:
    fs::path m_path;
};

} // namespace

void RunCore(const HostConfig& config, FrameSink& sink) {
    if (config.corePath.empty()) {
        throw std::runtime_error("no core library given");
    }

    // Saves always go to a fresh directory, so earlier runs never affect the result (as in brimir_bench).
    ScratchDir scratch;
    const fs::path saveDir = scratch.Path() / "save";
    fs::create_directories(saveDir);

    fs::path systemDir;
    if (config.systemDir.empty()) {
        systemDir = scratch.Path() / "system";
        fs::create_directories(systemDir);
    } else {
        systemDir = config.systemDir;
        std::error_code ec;
        if (!fs::is_directory(systemDir, ec)) {
            throw std::runtime_error("--system-dir is not an existing directory: " + config.systemDir);
        }
    }

    HostConfig effective = config;
    if (!config.biosPath.empty()) {
        const fs::path source = config.biosPath;
        std::error_code ec;
        if (!fs::is_regular_file(source, ec)) {
            throw std::runtime_error("BIOS not found: " + config.biosPath);
        }
        const std::string name = source.filename().string();
        fs::copy_file(source, systemDir / name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            throw std::runtime_error("cannot copy the BIOS into " + systemDir.string() + ": " + ec.message());
        }
        if (const char* option = BiosOptionForFile(name)) {
            // Pick exactly this BIOS even if the system directory holds others; --option can still override it.
            effective.options.insert(effective.options.begin(), {"brimir_bios", option});
        } else {
            std::fprintf(stderr, "vkhost: warning: '%s' is not a file name the core looks for\n", name.c_str());
        }
    }

    LibretroHost host;
    if (!host.Load(effective, systemDir.string(), saveDir.string())) {
        throw std::runtime_error("loading the core failed");
    }

    for (int i = 0; i < config.warmup; ++i) {
        host.RunFrame(nullptr);
    }
    for (int i = 0; i < config.frames; ++i) {
        host.RunFrame(&sink);
    }
    if (host.UnreadableFrames() > 0) {
        std::fprintf(stderr, "vkhost: %llu frames could not be read\n",
                     static_cast<unsigned long long>(host.UnreadableFrames()));
    }
    host.Unload();
}

std::vector<HostFrame> RunCore(const HostConfig& config) {
    struct VectorSink : FrameSink {
        std::vector<HostFrame> frames;
        void OnFrame(HostFrame&& frame) override { frames.push_back(std::move(frame)); }
    } sink;
    sink.frames.reserve(static_cast<size_t>(std::max(config.frames, 0)));
    RunCore(config, sink);
    return std::move(sink.frames);
}

} // namespace vkhost
