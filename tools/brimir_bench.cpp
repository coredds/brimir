// Brimir - headless frame benchmark
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Runs real content without a frontend and reports host ms/frame plus the
// share of time spent in each SH-2. See tools/README.md for usage.

#include <brimir/core_wrapper.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Args {
    std::string bios;
    std::string game;
    std::string state;
    std::string dumpState;
    int dumpAt = -1;
    int frames = 1800;
    int warmup = 120;
};

void PrintUsage() {
    std::puts(
        "Usage: brimir_bench --bios <file> [--game <cue|chd|ccd|mds|iso|m3u>] [--state <file>]\n"
        "                    [--frames N] [--warmup N]\n"
        "       brimir_bench --bios <file> [--game <file>] [--state <file>] --dump-at N --dump-state <file>\n"
        "\n"
        "  --bios        Saturn BIOS image (required)\n"
        "  --game        disc image to load (omit to benchmark the BIOS menu)\n"
        "  --state       save state produced by --dump-state (raw core state data)\n"
        "  --frames      measured frames (default 1800)\n"
        "  --warmup      unmeasured frames before measuring (default 120)\n"
        "  --dump-at     run N frames, write a save state to --dump-state, and exit\n");
}

bool ParseInt(const char* text, int& out) {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > 10'000'000) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool ParseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        if (i + 1 >= argc) {
            std::fprintf(stderr, "Missing value for %s\n", opt.c_str());
            return false;
        }
        const char* value = argv[++i];
        if (opt == "--bios") {
            args.bios = value;
        } else if (opt == "--game") {
            args.game = value;
        } else if (opt == "--state") {
            args.state = value;
        } else if (opt == "--dump-state") {
            args.dumpState = value;
        } else if (opt == "--dump-at") {
            if (!ParseInt(value, args.dumpAt)) {
                std::fprintf(stderr, "Invalid --dump-at value: %s\n", value);
                return false;
            }
        } else if (opt == "--frames") {
            if (!ParseInt(value, args.frames) || args.frames == 0) {
                std::fprintf(stderr, "Invalid --frames value: %s\n", value);
                return false;
            }
        } else if (opt == "--warmup") {
            if (!ParseInt(value, args.warmup)) {
                std::fprintf(stderr, "Invalid --warmup value: %s\n", value);
                return false;
            }
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", opt.c_str());
            return false;
        }
    }
    if (args.bios.empty()) {
        std::fprintf(stderr, "--bios is required\n");
        return false;
    }
    if ((args.dumpAt >= 0) != !args.dumpState.empty()) {
        std::fprintf(stderr, "--dump-at and --dump-state must be used together\n");
        return false;
    }
    return true;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

double Percentile(std::vector<double> sorted, double p) {
    std::sort(sorted.begin(), sorted.end());
    const size_t index = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
    return sorted[std::min(index, sorted.size() - 1)];
}

double AvgMs(const brimir::Profiler& profiler, const char* name) {
    const auto timing = profiler.GetTiming(name);
    return timing ? timing->avgMs() : 0.0;
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, args)) {
        PrintUsage();
        return 1;
    }

    brimir::CoreWrapper core;
    if (!core.Initialize()) {
        std::fprintf(stderr, "Failed to initialize the core\n");
        return 2;
    }
    if (!core.LoadIPLFromFile(args.bios.c_str())) {
        std::fprintf(stderr, "Failed to load BIOS: %s\n", args.bios.c_str());
        return 2;
    }

    if (!args.game.empty()) {
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec) / "brimir_bench";
        std::filesystem::create_directories(dir, ec);
        const std::string dirStr = dir.string();
        if (!core.LoadGame(args.game.c_str(), dirStr.c_str(), dirStr.c_str())) {
            std::fprintf(stderr, "Failed to load game: %s\n", core.GetLastError().c_str());
            return 2;
        }
    }

    if (!args.state.empty()) {
        std::vector<uint8_t> data;
        if (!ReadFile(args.state, data) || !core.LoadState(data.data(), data.size())) {
            std::fprintf(stderr, "Failed to load save state: %s\n", args.state.c_str());
            return 2;
        }
    }

    if (args.dumpAt >= 0) {
        for (int i = 0; i < args.dumpAt; ++i) {
            core.RunFrame();
        }
        std::vector<uint8_t> data(core.GetStateSize());
        if (!core.SaveState(data.data(), data.size())) {
            std::fprintf(stderr, "SaveState failed\n");
            return 2;
        }
        std::ofstream out(args.dumpState, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            std::fprintf(stderr, "Failed to write %s\n", args.dumpState.c_str());
            return 2;
        }
        std::printf("Wrote save state after %d frames: %s\n", args.dumpAt, args.dumpState.c_str());
        return 0;
    }

    for (int i = 0; i < args.warmup; ++i) {
        core.RunFrame();
    }

    core.SetProfilingEnabled(true);
    core.ResetProfiling();

    std::vector<double> frameMs;
    frameMs.reserve(static_cast<size_t>(args.frames));
    for (int i = 0; i < args.frames; ++i) {
        const auto start = std::chrono::steady_clock::now();
        core.RunFrame();
        const auto end = std::chrono::steady_clock::now();
        frameMs.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }

    double total = 0.0;
    for (double ms : frameMs) {
        total += ms;
    }
    const double avg = total / static_cast<double>(frameMs.size());

    const brimir::Profiler& profiler = core.GetProfiler();
    const double ymirMs = AvgMs(profiler, "Ymir_RunFrame");
    const double masterMs = AvgMs(profiler, "SH2_Master");
    const double slaveMs = AvgMs(profiler, "SH2_Slave");
    const auto share = [ymirMs](double ms) { return ymirMs > 0.0 ? 100.0 * ms / ymirMs : 0.0; };

    std::printf("content      : %s\n", args.game.empty() ? "(BIOS menu)" : args.game.c_str());
    std::printf("frames       : %d (warmup %d)\n", args.frames, args.warmup);
    std::printf("ms/frame     : avg %.3f  p50 %.3f  p95 %.3f  p99 %.3f  max %.3f\n", avg,
                Percentile(frameMs, 0.50), Percentile(frameMs, 0.95), Percentile(frameMs, 0.99),
                *std::max_element(frameMs.begin(), frameMs.end()));
    std::printf("fps (host)   : %.1f\n", avg > 0.0 ? 1000.0 / avg : 0.0);
    std::printf("Ymir_RunFrame: %.3f ms/frame\n", ymirMs);
    std::printf("SH2 master   : %.3f ms/frame (%.1f%% of Ymir_RunFrame)\n", masterMs, share(masterMs));
    std::printf("SH2 slave    : %.3f ms/frame (%.1f%% of Ymir_RunFrame)\n", slaveMs, share(slaveMs));
    std::printf("SH2 total    : %.3f ms/frame (%.1f%% of Ymir_RunFrame)\n", masterMs + slaveMs,
                share(masterMs + slaveMs));
    return 0;
}
