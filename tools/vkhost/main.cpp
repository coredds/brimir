// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Command line layer: parses the flags, runs the core through RunCore and prints/dumps the recorded frames.
// See tools/README.md for usage.

#include "compare.hpp"
#include "host.hpp"

#include "../frame_hash.hpp"

#include <algorithm>
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
    bool compare = false;
    int dumpFrame = -1; // --dump-frame I: with --dump-dir, write only recorded frame I
    std::vector<std::pair<std::string, std::string>> compareVulkanOptions;
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
        "  --vulkan      act as a Vulkan frontend: answer SET_HW_RENDER, run the context negotiation and read\n"
        "                back every set_image image (selects brimir_renderer=vulkan). Uses the validation layer\n"
        "                when installed; BRIMIR_VK_DEVICE picks the GPU\n"
        "  --refuse-hw   refuse SET_HW_RENDER, as RetroArch does on a gl/d3d driver without driver switching\n"
        "  --reset-at F  with --vulkan: driver reinit before frame F (counting warmup frames from 0):\n"
        "                context_destroy, a new device through the negotiation, context_reset\n"
        "  --reset-in-run F  the same reinit from inside frame F's retro_run (at the core's first environment\n"
        "                call), as RetroArch does inside SET_SYSTEM_AV_INFO (PAL content); a SET_SYSTEM_AV_INFO\n"
        "                that changes the frame rate or maximum geometry also triggers one\n"
        "  --runahead    run-ahead simulation: before each frame one hidden frame (video disabled; not rolled\n"
        "                back, so compare with a software --runahead run), and a second instance of the core\n"
        "                (refused a hardware context) running one frame per frame with audio and video disabled\n"
        "  --preferred-hw gl  with --vulkan: GET_PREFERRED_HW_RENDER reports GL (RetroArch on the gl driver with\n"
        "                driver switching allowed); SET_HW_RENDER would still be accepted\n"
        "  --fake-api X.Y  with --vulkan: vkGetPhysicalDeviceProperties tells the core the GPU supports only\n"
        "                Vulkan X.Y (for example 1.1: the core's copy-only output)\n"
        "  --grow-mask F  with --vulkan: sync index mask 0b1 until frame F, then 0b111 (device idle)\n"
        "  --fail-compute-on-grow  with --grow-mask: the core's vkCreateComputePipelines fails after the growth\n"
        "  --no-validation  with --vulkan/--compare: do not load the validation layer (timing runs)\n"
        "  --compare     run the same content on the software and the Vulkan renderer in two child processes and\n"
        "                compare the per-frame hashes; on the first difference report the frame, the first\n"
        "                differing pixel and both values and dump both frames as PPM (into --dump-dir, default a\n"
        "                kept temporary directory). Exit 3 on any difference, or when the Vulkan run did not use the\n"
        "                compute frame output for every frame. Other flags go to both runs\n"
        "  --compare-vulkan-option key=value  with --compare: core option for the Vulkan run only\n"
        "  --dump-frame I  with --dump-dir: write only recorded frame I\n"
        "  --help, -h    show this help\n"
        "\n"
        "Exit status: 0 ok, 1 usage, 2 error, 3 a check failed (queue lock, validation, hidden-frame GPU work,\n"
        "unreadable frames; see the 'problem:' lines), 4 --vulkan without a usable Vulkan device.\n",
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
        if (opt == "--vulkan") {
            args.config.vulkan = true;
            continue;
        }
        if (opt == "--refuse-hw") {
            args.config.refuseHw = true;
            continue;
        }
        if (opt == "--fail-compute-on-grow") {
            args.config.failComputeOnGrow = true;
            continue;
        }
        if (opt == "--runahead") {
            args.config.runahead = true;
            continue;
        }
        if (opt == "--compare") {
            args.compare = true;
            continue;
        }
        if (opt == "--no-validation") {
            args.config.noValidation = true;
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
        } else if (opt == "--dump-frame") {
            if (!ParseInt(value, args.dumpFrame)) {
                std::fprintf(stderr, "Invalid --dump-frame value: %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--compare-vulkan-option") {
            const std::string text = value;
            const size_t eq = text.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::fprintf(stderr, "Invalid --compare-vulkan-option (expected key=value): %s\n", value);
                return ParseResult::Error;
            }
            args.compareVulkanOptions.emplace_back(text.substr(0, eq), text.substr(eq + 1));
        } else if (opt == "--reset-at") {
            if (!ParseInt(value, args.config.resetAt)) {
                std::fprintf(stderr, "Invalid --reset-at value: %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--reset-in-run") {
            if (!ParseInt(value, args.config.resetInRun)) {
                std::fprintf(stderr, "Invalid --reset-in-run value: %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--grow-mask") {
            if (!ParseInt(value, args.config.growMaskAt)) {
                std::fprintf(stderr, "Invalid --grow-mask value: %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--preferred-hw") {
            const std::string hw = value;
            if (hw != "gl" && hw != "vulkan") {
                std::fprintf(stderr, "Invalid --preferred-hw value (gl or vulkan): %s\n", value);
                return ParseResult::Error;
            }
            args.config.preferGl = hw == "gl";
        } else if (opt == "--fake-api") {
            unsigned major = 0;
            unsigned minor = 0;
            if (std::sscanf(value, "%u.%u", &major, &minor) != 2 || major != 1 || minor > 9) {
                std::fprintf(stderr, "Invalid --fake-api value (1.N): %s\n", value);
                return ParseResult::Error;
            }
            args.config.fakeApi = (major << 22) | (minor << 12); // VK_MAKE_API_VERSION(0, major, minor, 0)
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
    // With --compare the Vulkan child gets --vulkan and the software child drops the Vulkan-only flags.
    const bool vulkanRun = args.config.vulkan || args.compare;
    if ((args.config.preferGl || args.config.fakeApi != 0 || args.config.growMaskAt >= 0 ||
         args.config.failComputeOnGrow) &&
        !vulkanRun) {
        std::fprintf(stderr, "--preferred-hw, --fake-api, --grow-mask and --fail-compute-on-grow need --vulkan\n");
        return ParseResult::Error;
    }
    if (args.config.noValidation && !vulkanRun) {
        std::fprintf(stderr, "--no-validation needs --vulkan or --compare\n");
        return ParseResult::Error;
    }
    if (!args.compare && !args.compareVulkanOptions.empty()) {
        std::fprintf(stderr, "--compare-vulkan-option needs --compare\n");
        return ParseResult::Error;
    }
    if (args.compare) {
        // The comparison is only meaningful when the Vulkan run really uses the compute output.
        if (args.config.refuseHw || args.config.preferGl || args.config.fakeApi != 0 || args.config.failComputeOnGrow) {
            std::fprintf(stderr, "--compare cannot be combined with --refuse-hw, --preferred-hw, --fake-api or "
                                 "--fail-compute-on-grow (they make the Vulkan run leave the compute output)\n");
            return ParseResult::Error;
        }
        auto overridesRenderer = [](const std::pair<std::string, std::string>& o) { return o.first == "brimir_renderer"; };
        if (std::any_of(args.config.options.begin(), args.config.options.end(), overridesRenderer) ||
            std::any_of(args.compareVulkanOptions.begin(), args.compareVulkanOptions.end(), overridesRenderer)) {
            std::fprintf(stderr, "--compare selects brimir_renderer itself; do not override it with --option\n");
            return ParseResult::Error;
        }
    }
    if ((args.config.resetAt >= 0 || args.config.resetInRun >= 0) && (!vulkanRun || args.config.refuseHw)) {
        std::fprintf(stderr, "--reset-at and --reset-in-run need --vulkan (without --refuse-hw)\n");
        return ParseResult::Error;
    }
    return ParseResult::Ok;
}

// Prints hashes and writes PPMs as the frames arrive, so long runs do not keep every frame in memory.
class CliSink : public vkhost::FrameSink {
public:
    CliSink(bool hash, fs::path dumpDir, int dumpFrame)
        : m_hash(hash), m_dumpDir(std::move(dumpDir)), m_dumpFrame(dumpFrame) {}

    void OnFrame(vkhost::HostFrame&& frame) override {
        if (m_hash) {
            const uint64_t hash = brimir_tools::HashFrame(frame.w, frame.h, frame.xrgb.data(), size_t{frame.w} * 4);
            std::printf("frame %zu %ux%u %016llx\n", m_count, frame.w, frame.h, static_cast<unsigned long long>(hash));
        }
        if (!m_dumpDir.empty() && (m_dumpFrame < 0 || static_cast<size_t>(m_dumpFrame) == m_count) &&
            !WritePpm(frame)) {
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
    int m_dumpFrame;
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

    if (args.compare) {
        vkhost::CompareConfig compare;
        compare.args.assign(argv + 1, argv + argc);
        compare.systemDir = args.config.systemDir;
        compare.dumpDir = args.config.dumpDir;
        compare.frames = args.config.frames;
        compare.vulkanOptions = args.compareVulkanOptions;
        return vkhost::RunCompare(compare);
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

    CliSink sink(args.hash, args.config.dumpDir, args.dumpFrame);
    vkhost::RunReport report;
    try {
        report = vkhost::RunCore(args.config, sink);
    } catch (const vkhost::VulkanUnavailable& e) {
        std::fprintf(stderr, "vkhost: Vulkan unavailable: %s\n", e.what());
        return 4;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 2;
    }
    std::fflush(stdout);
    std::fprintf(stderr, "vkhost: recorded %zu frames (%llu hardware), warmup %d\n", sink.Count(),
                 static_cast<unsigned long long>(report.recordedHardwareFrames), args.config.warmup);
    if (sink.Count() > 0) {
        // Wall time of the recorded frames' retro_run calls. Hashing/PPM writing (the sink) is left out; the
        // Vulkan host's readback of each frame (a wait for the GPU and a copy) is in the first figure only.
        const double perFrame = (report.recordSeconds - report.sinkSeconds) * 1000.0 / static_cast<double>(sink.Count());
        const double perFrameNoReadback =
            (report.recordSeconds - report.sinkSeconds - report.readbackSeconds) * 1000.0 / static_cast<double>(sink.Count());
        std::fprintf(stderr, "vkhost: timing: %.3f ms/frame (%.3f without the host readback), %zu frames\n", perFrame,
                     perFrameNoReadback, sink.Count());
    }
    if (args.config.vulkan) {
        std::fprintf(stderr,
                     "vkhost: vulkan: context resets %llu, core submits %llu, queue-lock violations %llu, "
                     "hidden-frame GPU calls %llu, validation %s, validation errors %u\n",
                     static_cast<unsigned long long>(report.contextResets),
                     static_cast<unsigned long long>(report.coreSubmits),
                     static_cast<unsigned long long>(report.lockViolations),
                     static_cast<unsigned long long>(report.hiddenGpuCalls), report.validation ? "on" : "off",
                     report.validationErrors);
    }
    if (args.config.runahead) {
        std::fprintf(stderr, "vkhost: runahead: second instance ran %llu frames, %llu hardware frames, context %s\n",
                     static_cast<unsigned long long>(report.secondaryFrames),
                     static_cast<unsigned long long>(report.secondaryHardwareFrames),
                     report.secondaryHadContext ? "yes" : "none");
    }
    for (const std::string& problem : report.problems) {
        std::fprintf(stderr, "vkhost: FAILED: %s\n", problem.c_str());
    }
    if (sink.Failed()) {
        return 2;
    }
    return report.problems.empty() ? 0 : 3;
}
