// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Command line layer: parses the flags, runs the core through RunCore and prints/dumps the recorded frames.
// See tools/README.md for usage.

#include "host.hpp"

#include "../frame_hash.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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

namespace {

namespace fs = std::filesystem;

enum class ParseResult { Ok, Help, Error };

struct CliArgs {
    vkhost::HostConfig config;
    bool hash = false;
};

void PrintUsage(std::FILE* out) {
    std::fputs(
        "Usage: brimir_vkhost (--bios <file> | --system-dir <dir>) [--game <cue|chd|ccd|mds|iso|m3u>]\n"
        "                     [--system-dir <dir>] [--core <library>] [--frames N] [--warmup N]\n"
        "                     [--option key=value]... [--hash] [--dump-dir <dir>] [--verbose]\n"
        "       brimir_vkhost --help\n"
        "\n"
        "Loads a libretro core dynamically, as RetroArch does, runs it without a window and records the\n"
        "frames it displays.\n"
        "\n"
        "  --core        core library (default: brimir_libretro next to this executable, or in ../lib)\n"
        "  --bios        BIOS image; copied into the system directory under its own file name and selected\n"
        "                through the brimir_bios option\n"
        "  --game        content to load (omit to run the BIOS on its own)\n"
        "  --system-dir  system directory (default: a fresh temporary directory). With --game it must hold\n"
        "                a configured brimir_saturn_rtc_<jp|us_eu>.smpc, see brimir_bench. The BIOS is copied\n"
        "                into it, so use a scratch copy. The save directory is always a fresh temporary one.\n"
        "  --frames      recorded frames (default 300)\n"
        "  --warmup      frames run before recording (default 120)\n"
        "  --option      core option override, for example --option brimir_overscan=1 (repeatable);\n"
        "                other options keep the default the core declares\n"
        "  --hash        print one line per recorded frame, 'frame <i> <w>x<h> <hash>' (64-bit FNV-1a of the\n"
        "                size and XRGB8888 pixels, the same as brimir_bench --hash)\n"
        "  --dump-dir    write every recorded frame as frame_<i>.ppm into this directory\n"
        "  --verbose     also log debug messages and geometry changes\n"
        "  --help, -h    show this help\n",
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

fs::path ExecutableDir() {
#ifdef _WIN32
    char buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (n > 0 && n < sizeof(buffer)) {
        return fs::path(std::string(buffer, n)).parent_path();
    }
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return exe.parent_path();
    }
#endif
    return fs::current_path();
}

ParseResult ParseArgs(int argc, char** argv, CliArgs& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        if (opt == "--help" || opt == "-h") {
            return ParseResult::Help;
        }
    }
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        if (opt == "--hash") { // options without a value
            args.hash = true;
            continue;
        }
        if (opt == "--verbose") {
            args.config.verbose = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "Missing value for %s\n", opt.c_str());
            return ParseResult::Error;
        }
        const char* value = argv[++i];
        if (opt == "--core") {
            args.config.corePath = value;
        } else if (opt == "--bios") {
            args.config.biosPath = value;
        } else if (opt == "--game") {
            args.config.gamePath = value;
        } else if (opt == "--system-dir") {
            args.config.systemDir = value;
        } else if (opt == "--dump-dir") {
            args.config.dumpDir = value;
        } else if (opt == "--frames") {
            if (!ParseInt(value, args.config.frames)) {
                std::fprintf(stderr, "Invalid --frames value: %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--warmup") {
            if (!ParseInt(value, args.config.warmup)) {
                std::fprintf(stderr, "Invalid --warmup value: %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--option") {
            const std::string text = value;
            const size_t eq = text.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::fprintf(stderr, "Invalid --option (expected key=value): %s\n", value);
                return ParseResult::Error;
            }
            args.config.options.emplace_back(text.substr(0, eq), text.substr(eq + 1));
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", opt.c_str());
            return ParseResult::Error;
        }
    }
    if (args.config.biosPath.empty() && args.config.systemDir.empty()) {
        std::fprintf(stderr, "--bios or --system-dir is required\n");
        return ParseResult::Error;
    }
    return ParseResult::Ok;
}

// Prints hashes and writes PPMs as the frames arrive, so long runs do not keep every frame in memory.
class CliSink : public vkhost::FrameSink {
public:
    CliSink(bool hash, fs::path dumpDir) : m_hash(hash), m_dumpDir(std::move(dumpDir)) {}

    void OnFrame(vkhost::HostFrame&& frame) override {
        if (m_hash) {
            const uint64_t hash = brimir_tools::HashFrame(frame.w, frame.h, frame.xrgb.data(), size_t{frame.w} * 4);
            std::printf("frame %zu %ux%u %016llx\n", m_count, frame.w, frame.h, static_cast<unsigned long long>(hash));
        }
        if (!m_dumpDir.empty() && !WritePpm(frame)) {
            m_failed = true;
        }
        ++m_count;
    }

    size_t Count() const { return m_count; }
    bool Failed() const { return m_failed; }

private:
    bool WritePpm(const vkhost::HostFrame& frame) const {
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%05zu.ppm", m_count);
        const fs::path path = m_dumpDir / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "P6\n" << frame.w << ' ' << frame.h << "\n255\n";
        std::vector<char> rgb(static_cast<size_t>(frame.w) * 3);
        for (uint32_t y = 0; y < frame.h; ++y) {
            for (uint32_t x = 0; x < frame.w; ++x) {
                const uint32_t px = frame.xrgb[static_cast<size_t>(y) * frame.w + x];
                rgb[x * 3 + 0] = static_cast<char>((px >> 16) & 0xFF);
                rgb[x * 3 + 1] = static_cast<char>((px >> 8) & 0xFF);
                rgb[x * 3 + 2] = static_cast<char>(px & 0xFF);
            }
            out.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
        }
        if (!out) {
            std::fprintf(stderr, "Failed to write %s\n", path.string().c_str());
            return false;
        }
        return true;
    }

    bool m_hash;
    fs::path m_dumpDir;
    size_t m_count = 0;
    bool m_failed = false;
};

} // namespace

int main(int argc, char** argv) {
    CliArgs args;
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

    if (args.config.corePath.empty()) {
#ifdef _WIN32
        const char* coreName = "brimir_libretro.dll";
#else
        const char* coreName = "brimir_libretro.so";
#endif
        // Windows builds put the DLL beside the executable, Unix builds in ../lib.
        const fs::path exeDir = ExecutableDir();
        fs::path core = exeDir / coreName;
        std::error_code ec;
        if (!fs::exists(core, ec) && fs::exists(exeDir.parent_path() / "lib" / coreName, ec)) {
            core = exeDir.parent_path() / "lib" / coreName;
        }
        args.config.corePath = core.string();
    }
    if (!args.config.dumpDir.empty()) {
        std::error_code ec;
        fs::create_directories(args.config.dumpDir, ec);
        if (ec) {
            std::fprintf(stderr, "Cannot create --dump-dir %s: %s\n", args.config.dumpDir.c_str(), ec.message().c_str());
            return 2;
        }
    }

    CliSink sink(args.hash, args.config.dumpDir);
    try {
        vkhost::RunCore(args.config, sink);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 2;
    }
    std::fflush(stdout);
    std::fprintf(stderr, "vkhost: recorded %zu frames (warmup %d)\n", sink.Count(), args.config.warmup);
    return sink.Failed() ? 2 : 0;
}
