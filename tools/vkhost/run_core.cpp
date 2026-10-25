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

RunReport RunCore(const HostConfig& config, FrameSink& sink) {
    if (config.corePath.empty()) {
        throw std::runtime_error("no core library given");
    }
    if ((config.resetAt >= 0 || config.resetInRun >= 0) && !config.vulkan) {
        throw std::runtime_error("--reset-at and --reset-in-run need --vulkan");
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

    if (config.vulkan) {
        // The renderer option the Vulkan host is for; an explicit --option still wins.
        effective.options.insert(effective.options.begin(), {"brimir_renderer", "vulkan"});
    }

    LibretroHost host;
    if (!host.Load(effective, systemDir.string(), saveDir.string())) {
        throw std::runtime_error("loading the core failed");
    }

    // Run-ahead as RetroArch does it for a hardware core, without the state transfers (retro_serialize /
    // retro_unserialize): unserializing a state taken after the BIOS's first peripheral polls corrupts the heap
    // (the save state's std::vector members are stored as raw bytes, task-6 report), so the hidden frames are
    // not rolled back and the comparison is against a software run with the same --runahead pattern.
    // - Single-instance run-ahead: before each displayed frame the main instance runs one hidden frame with video
    //   disabled (GET_AUDIO_VIDEO_ENABLE without RETRO_AV_ENABLE_VIDEO): no GPU work may happen in it.
    // - Second instance: RetroArch refuses SET_HW_RENDER (and the negotiation interface) there, so it runs on
    //   software; it is a separate copy of the library, and runs one frame with audio and video disabled per
    //   displayed frame.
    LibretroHost secondary;
    if (config.runahead) {
        fs::path copy = scratch.Path() / ("runahead_" + fs::path(config.corePath).filename().string());
        std::error_code ec;
        fs::copy_file(config.corePath, copy, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            throw std::runtime_error("cannot copy the core for the run-ahead instance: " + ec.message());
        }
        const fs::path secondSaveDir = scratch.Path() / "save2";
        fs::create_directories(secondSaveDir);
        HostConfig second = effective;
        second.corePath = copy.string();
        second.vulkan = false;
        second.refuseHw = true;
        if (!secondary.Load(second, systemDir.string(), secondSaveDir.string())) {
            throw std::runtime_error("loading the run-ahead instance failed");
        }
    }

    RunReport report;
    const int total = config.warmup + config.frames;
    uint64_t secondaryFrames = 0;
    for (int i = 0; i < total; ++i) {
        if (i == config.resetAt) {
            host.ResetContext();
        }
        if (config.runahead) {
            host.SetAvEnable(RETRO_AV_ENABLE_AUDIO);
            host.RunFrame(nullptr);
            host.SetAvEnable(RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO);
        }
        host.RunFrame(i >= config.warmup ? &sink : nullptr);
        if (config.runahead) {
            secondary.SetAvEnable(0);
            secondary.RunFrame(nullptr);
            ++secondaryFrames;
        }
    }

    if (config.runahead) {
        report.secondaryFrames = secondaryFrames;
        report.secondaryHardwareFrames = secondary.HardwareFrames();
        report.secondaryHadContext = secondary.HadContext();
        if (report.secondaryHardwareFrames != 0 || report.secondaryHadContext) {
            report.problems.push_back("the run-ahead instance got a hardware context or sent hardware frames");
        }
        secondary.Unload();
        RunReport secondaryReport;
        secondary.FillReport(secondaryReport);
        report.problems.insert(report.problems.end(), secondaryReport.problems.begin(),
                               secondaryReport.problems.end());
    }
    host.Unload();
    host.FillReport(report);
    if (report.unreadableFrames > 0) {
        report.problems.push_back(std::to_string(report.unreadableFrames) + " frames could not be read");
    }
    if (report.validationErrors > 0) {
        report.problems.push_back(std::to_string(report.validationErrors) + " Vulkan validation errors");
    }
    if (report.lockViolations > 0) {
        report.problems.push_back(std::to_string(report.lockViolations) + " queue-lock violations");
    }
    if (report.hiddenGpuCalls > 0) {
        report.problems.push_back(std::to_string(report.hiddenGpuCalls) + " GPU calls in frames with video disabled");
    }
    return report;
}

std::vector<HostFrame> RunCore(const HostConfig& config, RunReport* report) {
    struct VectorSink : FrameSink {
        std::vector<HostFrame> frames;
        void OnFrame(HostFrame&& frame) override { frames.push_back(std::move(frame)); }
    } sink;
    sink.frames.reserve(static_cast<size_t>(std::max(config.frames, 0)));
    RunReport result = RunCore(config, sink);
    if (report) {
        *report = std::move(result);
    }
    return std::move(sink.frames);
}

} // namespace vkhost
