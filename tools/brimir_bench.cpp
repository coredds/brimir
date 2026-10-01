// Brimir - headless frame benchmark
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Runs real content without a frontend and reports host ms/frame plus the
// share of time spent in each SH-2. See tools/README.md for usage.

#include <brimir/core_wrapper.hpp>
#include <brimir/jit/executor.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct Args {
    std::string bios;
    std::string game;
    std::string state;
    std::string dumpState;
    std::string systemDir;
    int dumpAt = -1;
    int frames = 1800;
    int warmup = 120;
    bool sh2Jit = false;
};

enum class ParseResult { Ok, Help, Error };

void PrintUsage(std::FILE* out) {
    std::fputs(
        "Usage: brimir_bench --bios <file> [--game <cue|chd|ccd|mds|iso|m3u>] [--system-dir <dir>]\n"
        "                    [--state <file>] [--frames N] [--warmup N] [--sh2-jit]\n"
        "       brimir_bench --bios <file> [--game <file>] [--system-dir <dir>] [--state <file>]\n"
        "                    [--sh2-jit] --dump-at N --dump-state <file>\n"
        "       brimir_bench --help\n"
        "\n"
        "  --bios        Saturn BIOS image (required)\n"
        "  --game        disc image to load (omit to benchmark the BIOS menu)\n"
        "  --system-dir  directory holding brimir_saturn_rtc_<jp|us_eu>.smpc (BIOS language/clock\n"
        "                settings). The suffix follows the BIOS region, so a configured file is\n"
        "                needed for each BIOS region used. Without one the BIOS stops at its\n"
        "                first-boot setup screen and never boots the disc. The core may write\n"
        "                RTC files back into this directory (including brimir_saturn_rtc_none.smpc),\n"
        "                so prefer a scratch copy over the real RetroArch system folder.\n"
        "                Only used with --game. Default: a fresh per-run temp directory\n"
        "                (unconfigured).\n"
        "  --state       save state produced by --dump-state (raw core state data)\n"
        "  --frames      measured frames (default 1800)\n"
        "  --warmup      unmeasured frames before measuring (default 120)\n"
        "  --dump-at     run N frames, write a save state to --dump-state, and exit\n"
        "  --sh2-jit     run both SH-2s through the experimental JIT (default: interpreter)\n"
        "  --help, -h    show this help\n"
        "\n"
        "Backup RAM (.srm) and cartridge RAM (.cart) go to a fresh temp directory that is\n"
        "removed on exit, so every run starts from the same state.\n",
        out);
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

ParseResult ParseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        if (opt == "--help" || opt == "-h") {
            return ParseResult::Help;
        }
    }
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        if (opt == "--sh2-jit") { // the only option without a value
            args.sh2Jit = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "Missing value for %s\n", opt.c_str());
            return ParseResult::Error;
        }
        const char* value = argv[++i];
        if (opt == "--bios") {
            args.bios = value;
        } else if (opt == "--game") {
            args.game = value;
        } else if (opt == "--system-dir") {
            args.systemDir = value;
        } else if (opt == "--state") {
            args.state = value;
        } else if (opt == "--dump-state") {
            args.dumpState = value;
        } else if (opt == "--dump-at") {
            if (!ParseInt(value, args.dumpAt)) {
                std::fprintf(stderr, "Invalid --dump-at value: %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--frames") {
            if (!ParseInt(value, args.frames) || args.frames == 0) {
                std::fprintf(stderr, "Invalid --frames value: %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--warmup") {
            if (!ParseInt(value, args.warmup)) {
                std::fprintf(stderr, "Invalid --warmup value: %s\n", value);
                return ParseResult::Error;
            }
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", opt.c_str());
            return ParseResult::Error;
        }
    }
    if (args.bios.empty()) {
        std::fprintf(stderr, "--bios is required\n");
        return ParseResult::Error;
    }
    if ((args.dumpAt >= 0) != !args.dumpState.empty()) {
        std::fprintf(stderr, "--dump-at and --dump-state must be used together\n");
        return ParseResult::Error;
    }
    return ParseResult::Ok;
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

// Creates a fresh, uniquely named directory under <temp>/brimir_bench for this
// run's backup RAM (.srm) and cartridge RAM (.cart). Returns false on error.
bool CreateRunDirectory(std::filesystem::path& out) {
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (ec) {
        std::fprintf(stderr, "Cannot determine the temp directory: %s\n", ec.message().c_str());
        return false;
    }
    const auto base = temp / "brimir_bench";
    std::filesystem::create_directories(base, ec);
    if (ec) {
        std::fprintf(stderr, "Cannot create %s: %s\n", base.string().c_str(), ec.message().c_str());
        return false;
    }
    std::random_device rd;
    const auto now = static_cast<unsigned long long>(
        std::chrono::system_clock::now().time_since_epoch().count());
    for (int attempt = 0; attempt < 16; ++attempt) {
        const unsigned long long nonce = (static_cast<unsigned long long>(rd()) << 32) ^ rd() ^ now;
        char name[40];
        std::snprintf(name, sizeof(name), "run-%016llx", nonce);
        const auto dir = base / name;
        // create_directory returns false (without error) if it already exists.
        if (std::filesystem::create_directory(dir, ec)) {
            out = dir;
            return true;
        }
        if (ec) {
            std::fprintf(stderr, "Cannot create %s: %s\n", dir.string().c_str(), ec.message().c_str());
            return false;
        }
    }
    std::fprintf(stderr, "Cannot create a unique run directory under %s\n", base.string().c_str());
    return false;
}

int Run(const Args& args, const std::filesystem::path& saveDir, const std::filesystem::path& systemDir);

} // namespace

int main(int argc, char** argv) {
    Args args;
    switch (ParseArgs(argc, argv, args)) {
    case ParseResult::Help:
        PrintUsage(stdout);
        return 0;
    case ParseResult::Error:
        PrintUsage(stderr);
        return 1;
    case ParseResult::Ok:
        break;
    }

    if (!args.systemDir.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(args.systemDir, ec)) {
            std::fprintf(stderr, "--system-dir is not an existing directory: %s\n", args.systemDir.c_str());
            return 2;
        }
    }

    std::filesystem::path runDir;
    if (!CreateRunDirectory(runDir)) {
        return 2;
    }
    // Removes runDir on every exit path, including exceptions from Run.
    struct RunDirGuard {
        const std::filesystem::path& dir;
        ~RunDirGuard() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            if (ec) {
                std::fprintf(stderr, "Warning: could not remove %s: %s\n", dir.string().c_str(),
                             ec.message().c_str());
            }
        }
    } runDirGuard{runDir};
    const std::filesystem::path systemDir = args.systemDir.empty() ? runDir : std::filesystem::path(args.systemDir);

    // The core is destroyed inside Run (also during unwinding), so any files
    // it writes on shutdown land in runDir before it is removed.
    try {
        return Run(args, runDir, systemDir);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 2;
    } catch (...) {
        std::fprintf(stderr, "Error: unknown exception\n");
        return 2;
    }
}

namespace {

int Run(const Args& args, const std::filesystem::path& saveDir, const std::filesystem::path& systemDir) {
    brimir::CoreWrapper core;
    core.SetSH2JitEnabled(args.sh2Jit);
    if (!core.Initialize()) {
        std::fprintf(stderr, "Failed to initialize the core\n");
        return 2;
    }
    if (!core.LoadIPLFromFile(args.bios.c_str())) {
        std::fprintf(stderr, "Failed to load BIOS: %s\n", args.bios.c_str());
        return 2;
    }

    if (!args.game.empty()) {
        const std::string saveStr = saveDir.string();
        const std::string systemStr = systemDir.string();
        if (!core.LoadGame(args.game.c_str(), saveStr.c_str(), systemStr.c_str())) {
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
    std::printf("sh2 jit      : %s\n", args.sh2Jit ? "on" : "off");
    std::printf("ms/frame     : avg %.3f  p50 %.3f  p95 %.3f  p99 %.3f  max %.3f\n", avg,
                Percentile(frameMs, 0.50), Percentile(frameMs, 0.95), Percentile(frameMs, 0.99),
                *std::max_element(frameMs.begin(), frameMs.end()));
    std::printf("fps (host)   : %.1f\n", avg > 0.0 ? 1000.0 / avg : 0.0);
    std::printf("Ymir_RunFrame: %.3f ms/frame\n", ymirMs);
    std::printf("SH2 master   : %.3f ms/frame (%.1f%% of Ymir_RunFrame)\n", masterMs, share(masterMs));
    std::printf("SH2 slave    : %.3f ms/frame (%.1f%% of Ymir_RunFrame)\n", slaveMs, share(slaveMs));
    std::printf("SH2 total    : %.3f ms/frame (%.1f%% of Ymir_RunFrame)\n", masterMs + slaveMs,
                share(masterMs + slaveMs));
    if (args.sh2Jit) {
        // Totals since initialization (warmup included).
        for (const bool master : {true, false}) {
            const brimir::jit::Executor* exec = core.GetSH2JitExecutor(master);
            if (exec != nullptr) {
                const auto& stats = exec->GetStats();
                std::printf("jit %-9s: blocksRun %llu  interpreted %llu\n", master ? "master" : "slave",
                            static_cast<unsigned long long>(stats.blocksRun),
                            static_cast<unsigned long long>(stats.interpreted));
            }
        }
    }
    return 0;
}

} // namespace
