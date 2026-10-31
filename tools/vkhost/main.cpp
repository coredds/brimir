// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Command line layer: parses the flags, runs the core through RunCore and prints/dumps the recorded frames.
// See tools/README.md for usage.

#include "compare.hpp"
#include "host.hpp"
#include "sample_points.hpp"

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
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {

namespace fs = std::filesystem;

enum class ParseResult { Ok, Help, Error };

using vkhost::Region;

struct CliArgs {
    vkhost::HostConfig config;
    bool hash = false;
    bool compare = false;
    int dumpFrame = -1; // --dump-frame I: with --dump-dir, write only recorded frame I
    int dumpEvery = 0;  // --dump-every N: with --dump-dir, write only recorded frames 0, N, 2N, ...
    std::vector<std::pair<std::string, std::string>> compareVulkanOptions;
    int compareState = 0; // --compare-state N (0: off)
    int hashSample = 0;  // --hash-sample N (0: off)
    int sharpness = 0;   // --sharpness N (0: off)
    Region region;       // --region x,y,w,h
    bool hasRegion = false;
    uint32_t rotation = 0; // brimir_rotation from --option (for the sample-point mapping)
    std::string fixedDate; // --fixed-date [date]: the BRIMIR_RTC_FIXED value ("" = off)
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
        "  --reset-in-present F  the same reinit at frame F's GET_AUDIO_VIDEO_ENABLE: after the core emulated the\n"
        "                frame and before it records and presents it\n"
        "  --hide-every N  before every Nth frame (counting warmup frames from 0, frames N-1, 2N-1, ...), one extra\n"
        "                frame with video disabled (not recorded; compare with a software run with the same N)\n"
        "  --state-roundtrip N  retro_serialize + retro_unserialize after every Nth frame\n"
        "  --save-state-at F <file>  retro_serialize after frame F (counting warmup frames from 0) into <file>; with\n"
        "                --compare each child writes <file>.software or <file>.vulkan\n"
        "  --save-state-every N <dir>  retro_serialize after every Nth recorded frame (recorded frames N-1, 2N-1, ...)\n"
        "                into <dir>/state_<recorded frame>.bin (not with --compare: see --compare-state)\n"
        "  --load-state <file>  retro_unserialize <file> before the first frame (after the context exists)\n"
        "  --runahead    run-ahead simulation: before each frame one hidden frame (video disabled; not rolled\n"
        "                back, so compare with a software --runahead run), and a second instance of the core\n"
        "                (refused a hardware context) running one frame per frame with audio and video disabled\n"
        "  --preferred-hw gl  with --vulkan: GET_PREFERRED_HW_RENDER reports GL (RetroArch on the gl driver with\n"
        "                driver switching allowed); SET_HW_RENDER would still be accepted\n"
        "  --fake-api X.Y  with --vulkan: vkGetPhysicalDeviceProperties tells the core the GPU supports only\n"
        "                Vulkan X.Y (for example 1.1: the core's copy-only output)\n"
        "  --grow-mask F  with --vulkan: sync index mask 0b1 until frame F, then 0b111 (device idle)\n"
        "  --fail-compute-on-grow  with --grow-mask: the core's vkCreateComputePipelines fails after the growth\n"
        "  --fail-compute-on-reset  with --vulkan: the same for every context after the first (a reinit ends in\n"
        "                copy-only)\n"
        "  --no-validation  with --vulkan/--compare: do not load the validation layer (timing runs)\n"
        "  --probe       collect per-frame counters while recording and print a summary. With --vulkan, the\n"
        "                Vulkan VDP renderer's VDP2 counters (sets BRIMIR_VDP2_PROBE for the core): per feature the\n"
        "                frames using it, batches, arena and copy bytes per frame (median, maximum) and the\n"
        "                front half's CPU time per frame. With a core built with -DBRIMIR_VDP1_PROBE=ON (any\n"
        "                renderer; sets BRIMIR_VDP1_PROBE), per retro_run the software VDP1 work: commands, lines,\n"
        "                pixel writes, VDP1SyncFB calls (and those after a draw or erase), CPU framebuffer writes\n"
        "                and erases, and the GPU VDP1 front half's draw batches by end reason (zero until the\n"
        "                renderer runs the front half) (median, maximum, mean)\n"
        "  --gpu-vdp1    with --vulkan/--compare: VDP1 on the GPU in the Vulkan VDP renderer (the core's default; sets\n"
        "                BRIMIR_VDP1_GPU=1; its log says 'Vulkan VDP1 renderer active')\n"
        "  --software-vdp1  with --vulkan/--compare: VDP1 in software inside the Vulkan VDP renderer, as in stage 1\n"
        "                (sets BRIMIR_VDP1_GPU=0, the core's debug override)\n"
        "  --compare     run the same content on the software and the Vulkan renderer in two child processes and\n"
        "                compare the per-frame hashes; on the first difference report the frame, the first\n"
        "                differing pixel and both values and dump both frames as PPM (into --dump-dir, default a\n"
        "                kept temporary directory). Exit 3 on any difference, or when the Vulkan run did not use the\n"
        "                compute frame output and the Vulkan VDP2 renderer for every frame. Other flags go to both\n"
        "                runs\n"
        "  --compare-vulkan-option key=value  with --compare: core option for the Vulkan run only\n"
        "  --compare-state N  with --compare (N <= --frames): both runs save a state after every Nth recorded frame and each pair is\n"
        "                diffed with brimir_state_diff (next to this executable): the VDP1 framebuffers (vdp.FBRAM,\n"
        "                displayFB) strictly, the renderer section (doubleV, mesh FBRAM, ...) up to its allowlist of\n"
        "                line-local fetcher caches. Any differing pair fails the compare (exit 3); the first one's diff\n"
        "                is printed and both states are copied into the dump directory\n"
        "  --dump-frame I  with --dump-dir: write only recorded frame I\n"
        "  --fixed-date [date]  pin the RTC for deterministic runs from boot: sets BRIMIR_RTC_FIXED for the core (and\n"
        "                the --compare children), which switches the RTC to the virtual clock at the date after loading\n"
        "                the content (log: 'RTC fixed to ...'). Without a date (or 1) brimir_bench's lockstep date,\n"
        "                1994-11-22 00:00:00; or YYYY-MM-DD[THH:MM:SS]. A --load-state state carries its own RTC.\n"
        "                With --compare-state every section of each state pair must match (brimir_state_diff --strict)\n"
        "  --dump-every N  with --dump-dir: write only recorded frames 0, N, 2N, ... (not with --dump-frame)\n"
        "  --internal-resolution N  the same as --option brimir_internal_resolution=N (1-4)\n"
        "  --set-option-at F key=value  a live option change before frame F (counting warmup frames from 0), reported\n"
        "                to the core by GET_VARIABLE_UPDATE (repeatable)\n"
        "  --hash-sample N  with --hash: hash the native image of the sample points instead of the frame. Frames are at\n"
        "                internal resolution N until the core logs another (\"Internal resolution: frames at Mx\"); the\n"
        "                sample point of native output pixel (x, y) is mapped through the rotation (brimir_rotation\n"
        "                from --option) to the N x frame. Conservatively excluded (hashed as 0), every frame: odd native\n"
        "                columns of hi-res frames and odd native rows of interlaced frames (see tools/README.md). The\n"
        "                line is 'frame <i> <native w>x<native h> <hash>'. With --compare the software run gets\n"
        "                --hash-sample 1 (the same exclusions), so Vulkan at N is compared with software at 1x\n"
        "  --sharpness N  per recorded frame (internal resolution as for --hash-sample), print the share of the N x N\n"
        "                blocks that are not constant and the mean absolute difference from the nearest upscale of\n"
        "                the sample points (tools/README.md), and a summary\n"
        "  --region x,y,w,h  with --sharpness: only these native output pixels\n"
        "  --fail-alloc-above MiB  with --vulkan: the core's vkAllocateMemory fails for larger allocations (fault\n"
        "                injection: a GPU short of memory)\n"
        "  --fail-output-images-above N  with --vulkan: the core's vkCreateImage fails for its output images\n"
        "                (MUTABLE_FORMAT) wider than 352 x N (a low-res frame's maximum width at N x)\n"
        "  --fail-output-images-on-grow  with --grow-mask and --fail-output-images-above: the limit applies only\n"
        "                once the mask has grown\n"
        "  --fail-submit-at F [--fail-submits N]  with --vulkan: the core's frame submit fails in runs F .. F+N-1\n"
        "                (counting warmup frames from 0; N defaults to 1): the core repeats the frontend's image\n"
        "  --defer-image-read N  with --vulkan: read each set_image image N (1-2) displayed frames later, as a\n"
        "                threaded frontend does (before the core's next wait_sync_index for its sync index); the core\n"
        "                destroying such an image or its view before the read fails the run\n"
        "  --help, -h    show this help\n"
        "\n"
        "Exit status: 0 ok, 1 usage, 2 error, 3 a check failed (queue lock, validation, presentation in a hidden\n"
        "frame, more than one submit in a retro_run, unreadable frames, state save/load; see the 'problem:' lines),\n"
        "4 --vulkan without a usable Vulkan device.\n",
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
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        std::error_code ec;
        const fs::path exe = fs::canonical(buffer, ec);
        return (ec ? fs::path(buffer) : exe).parent_path();
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
        if (opt == "--fail-output-images-on-grow") {
            args.config.failOutputImagesOnGrow = true;
            continue;
        }
        if (opt == "--fail-compute-on-reset") {
            args.config.failComputeOnReset = true;
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
        if (opt == "--probe") {
            args.config.probe = true;
            continue;
        }
        if (opt == "--gpu-vdp1") {
            args.config.gpuVdp1 = true;
            continue;
        }
        if (opt == "--software-vdp1") {
            args.config.softwareVdp1 = true;
            continue;
        }
        if (opt == "--fixed-date") { // an optional value
            args.fixedDate = "1";
            if (i + 1 < argc && vkhost::IsFixedDateValue(argv[i + 1])) {
                args.fixedDate = argv[++i];
            }
            continue;
        }
        if (opt == "--save-state-at") { // two values
            if (i + 2 >= argc || !ParseInt(argv[i + 1], args.config.saveStateAt)) {
                std::fprintf(stderr, "--save-state-at needs a frame number and a file\n");
                return ParseResult::Error;
            }
            args.config.saveStatePath = argv[i + 2];
            i += 2;
            continue;
        }
        if (opt == "--save-state-every") { // two values
            if (i + 2 >= argc || !ParseInt(argv[i + 1], args.config.saveStateEvery) || args.config.saveStateEvery == 0) {
                std::fprintf(stderr, "--save-state-every needs a frame count (1 or more) and a directory\n");
                return ParseResult::Error;
            }
            args.config.saveStateDir = argv[i + 2];
            i += 2;
            continue;
        }
        if (opt == "--set-option-at") { // two values
            vkhost::HostConfig::OptionChange change;
            const std::string text = i + 2 < argc ? argv[i + 2] : "";
            const size_t eq = text.find('=');
            if (i + 2 >= argc || !ParseInt(argv[i + 1], change.frame) || eq == std::string::npos || eq == 0) {
                std::fprintf(stderr, "--set-option-at needs a frame number and key=value\n");
                return ParseResult::Error;
            }
            change.key = text.substr(0, eq);
            change.value = text.substr(eq + 1);
            args.config.optionChanges.push_back(change);
            i += 2;
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
        } else if (opt == "--dump-every") {
            if (!ParseInt(value, args.dumpEvery) || args.dumpEvery == 0) {
                std::fprintf(stderr, "Invalid --dump-every value (1 or more): %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--compare-state") {
            if (!ParseInt(value, args.compareState) || args.compareState == 0) {
                std::fprintf(stderr, "Invalid --compare-state value (1 or more): %s\n", value);
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
        } else if (opt == "--reset-in-present") {
            if (!ParseInt(value, args.config.resetInPresent)) {
                std::fprintf(stderr, "Invalid --reset-in-present value: %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--hide-every") {
            if (!ParseInt(value, args.config.hideEvery) || args.config.hideEvery == 0) {
                std::fprintf(stderr, "Invalid --hide-every value (1 or more): %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--state-roundtrip") {
            if (!ParseInt(value, args.config.stateRoundtrip) || args.config.stateRoundtrip == 0) {
                std::fprintf(stderr, "Invalid --state-roundtrip value (1 or more): %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--load-state") {
            args.config.loadStatePath = value;
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
        } else if (opt == "--internal-resolution") {
            int n = 0;
            if (!ParseInt(value, n) || n < 1 || n > 4) {
                std::fprintf(stderr, "Invalid --internal-resolution value (1-4): %s\n", value);
                return ParseResult::Error;
            }
            args.config.options.emplace_back("brimir_internal_resolution", std::to_string(n));
        } else if (opt == "--hash-sample" || opt == "--sharpness") {
            int n = 0;
            if (!ParseInt(value, n) || n < 1 || n > 4) {
                std::fprintf(stderr, "Invalid %s value (1-4): %s\n", opt.c_str(), value);
                return ParseResult::Error;
            }
            (opt == "--hash-sample" ? args.hashSample : args.sharpness) = n;
        } else if (opt == "--region") {
            unsigned x = 0, y = 0, w = 0, h = 0;
            if (std::sscanf(value, "%u,%u,%u,%u", &x, &y, &w, &h) != 4 || w == 0 || h == 0) {
                std::fprintf(stderr, "Invalid --region value (x,y,w,h with w, h > 0): %s\n", value);
                return ParseResult::Error;
            }
            args.region = {x, y, w, h};
            args.hasRegion = true;
        } else if (opt == "--defer-image-read") {
            if (!ParseInt(value, args.config.deferImageRead) || args.config.deferImageRead < 1 ||
                args.config.deferImageRead > 2) {
                std::fprintf(stderr, "Invalid --defer-image-read value (1 or 2): %s\n", value);
                return ParseResult::Error;
            }
        } else if (opt == "--fail-submit-at" || opt == "--fail-submits") {
            int n = 0;
            if (!ParseInt(value, n) || (opt == "--fail-submits" && n < 1)) {
                std::fprintf(stderr, "Invalid %s value: %s\n", opt.c_str(), value);
                return ParseResult::Error;
            }
            (opt == "--fail-submit-at" ? args.config.failSubmitAt : args.config.failSubmits) = n;
        } else if (opt == "--fail-output-images-above") {
            int n = 0;
            if (!ParseInt(value, n) || n < 1 || n > 3) {
                std::fprintf(stderr, "Invalid --fail-output-images-above value (1-3): %s\n", value);
                return ParseResult::Error;
            }
            args.config.failOutputImagesAbove = static_cast<uint32_t>(n);
        } else if (opt == "--fail-alloc-above") {
            int mib = 0;
            if (!ParseInt(value, mib) || mib == 0) {
                std::fprintf(stderr, "Invalid --fail-alloc-above value (MiB, 1 or more): %s\n", value);
                return ParseResult::Error;
            }
            args.config.failAllocAboveMiB = static_cast<uint64_t>(mib);
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
         args.config.failComputeOnGrow || args.config.failComputeOnReset) &&
        !vulkanRun) {
        std::fprintf(stderr, "--preferred-hw, --fake-api, --grow-mask and --fail-compute-on-grow need --vulkan\n");
        return ParseResult::Error;
    }
    if ((args.config.failAllocAboveMiB != 0 || args.config.failOutputImagesAbove != 0 ||
         args.config.deferImageRead != 0 || args.config.failSubmitAt >= 0) &&
        !vulkanRun) {
        std::fprintf(stderr, "--fail-alloc-above, --fail-output-images-above, --fail-submit-at and "
                             "--defer-image-read need --vulkan or --compare\n");
        return ParseResult::Error;
    }
    if (args.config.failOutputImagesOnGrow &&
        (args.config.growMaskAt < 0 || args.config.failOutputImagesAbove == 0)) {
        std::fprintf(stderr, "--fail-output-images-on-grow needs --grow-mask and --fail-output-images-above\n");
        return ParseResult::Error;
    }
    if (args.config.deferImageRead != 0 && args.config.growMaskAt >= 0) {
        // The ring starts at one slot: a frontend could not keep any image past the next frame.
        std::fprintf(stderr, "--defer-image-read cannot be combined with --grow-mask\n");
        return ParseResult::Error;
    }
    if (args.hasRegion && args.sharpness == 0) {
        std::fprintf(stderr, "--region needs --sharpness\n");
        return ParseResult::Error;
    }
    if (args.hashSample != 0 && !args.hash && !args.compare) {
        std::fprintf(stderr, "--hash-sample needs --hash (or --compare)\n");
        return ParseResult::Error;
    }
    for (const auto& [key, value] : args.config.options) {
        if (key == "brimir_rotation") {
            const int degrees = std::atoi(value.c_str());
            args.rotation = degrees == 90 || degrees == 180 || degrees == 270 ? static_cast<uint32_t>(degrees) : 0;
        }
    }
    if (args.config.probe && args.compare) {
        std::fprintf(stderr, "--probe cannot be combined with --compare\n");
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
    if (!args.compare && args.compareState != 0) {
        std::fprintf(stderr, "--compare-state needs --compare\n");
        return ParseResult::Error;
    }
    if (args.compareState > args.config.frames) {
        std::fprintf(stderr, "--compare-state %d saves no state in %d recorded frames (needs N <= --frames)\n",
                     args.compareState, args.config.frames);
        return ParseResult::Error;
    }
    if (args.dumpEvery != 0 && args.dumpFrame >= 0) {
        std::fprintf(stderr, "--dump-every and --dump-frame cannot be combined\n");
        return ParseResult::Error;
    }
    if (args.compare && args.config.saveStateEvery != 0) {
        std::fprintf(stderr, "--save-state-every cannot be combined with --compare (use --compare-state)\n");
        return ParseResult::Error;
    }
    if (args.compare) {
        // The comparison is only meaningful when the Vulkan run really uses the compute output.
        if (args.config.refuseHw || args.config.preferGl || args.config.fakeApi != 0 || args.config.failComputeOnGrow ||
            args.config.failComputeOnReset) {
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
    if ((args.config.resetAt >= 0 || args.config.resetInRun >= 0 || args.config.resetInPresent >= 0) &&
        (!vulkanRun || args.config.refuseHw)) {
        std::fprintf(stderr, "--reset-at, --reset-in-run and --reset-in-present need --vulkan (without --refuse-hw)\n");
        return ParseResult::Error;
    }
    return ParseResult::Ok;
}

// Prints hashes and writes PPMs as the frames arrive, so long runs do not keep every frame in memory.
class CliSink : public vkhost::FrameSink {
public:
    CliSink(bool hash, fs::path dumpDir, int dumpFrame)
        : m_hash(hash), m_dumpDir(std::move(dumpDir)), m_dumpFrame(dumpFrame) {}

    // --hash-sample / --sharpness: the internal resolution assumed until the core logs one, the output rotation.
    void SetSampling(uint32_t hashSample, uint32_t sharpness, uint32_t rotation, Region region) {
        m_hashSample = hashSample;
        m_sharpness = sharpness;
        m_rotation = rotation;
        m_region = region;
    }

    // --dump-every N: only recorded frames 0, N, 2N, ... (without --dump-frame)
    void SetDumpEvery(int n) { m_dumpEvery = n; }

    void OnFrame(vkhost::HostFrame&& frame) override {
        if (m_hash && m_hashSample != 0) {
            HashSamples(frame);
        } else if (m_hash) {
            const uint64_t hash = brimir_tools::HashFrame(frame.w, frame.h, frame.xrgb.data(), size_t{frame.w} * 4);
            std::printf("frame %zu %ux%u %016llx\n", m_count, frame.w, frame.h, static_cast<unsigned long long>(hash));
        }
        if (m_sharpness != 0) {
            Sharpness(frame);
        }
        const bool dumpThis = m_dumpFrame >= 0 ? static_cast<size_t>(m_dumpFrame) == m_count
                              : m_dumpEvery > 0 ? m_count % static_cast<size_t>(m_dumpEvery) == 0
                                                : true;
        if (!m_dumpDir.empty() && dumpThis && !WritePpm(frame)) {
            m_failed = true;
        }
        ++m_count;
    }

    size_t Count() const { return m_count; }
    bool Failed() const { return m_failed; }

    // --hash-sample: how much of the frames the hashes covered (the rest is the excluded lattice).
    void PrintSampleSummary() const {
        if (m_sampleFrames == 0) {
            return;
        }
        std::fprintf(stderr, "vkhost: hash-sample: sampled %llu of %llu points over %zu frames (%.1f%%)\n",
                     static_cast<unsigned long long>(m_sampledPoints), static_cast<unsigned long long>(m_samplePoints),
                     m_sampleFrames,
                     m_samplePoints != 0 ? 100.0 * static_cast<double>(m_sampledPoints) / static_cast<double>(m_samplePoints)
                                         : 0.0);
    }

    void PrintSharpnessSummary() const {
        if (m_sharpness == 0) {
            return;
        }
        const double frames = static_cast<double>(std::max<size_t>(m_sharpFrames, 1));
        std::fprintf(stderr,
                     "vkhost: sharpness: %zu frames (%zu at more than 1x), mean non-constant blocks %.2f%%, mean abs "
                     "diff %.4f\n",
                     m_sharpFrames, m_sharpScaledFrames, m_sumNonConstant * 100.0 / frames, m_sumMeanDiff / frames);
    }

private:
    uint32_t FrameScale(const vkhost::HostFrame& frame, uint32_t fallback) const {
        return frame.scale != 0 ? frame.scale : fallback;
    }

    void HashSamples(const vkhost::HostFrame& frame) {
        const uint32_t n = FrameScale(frame, m_hashSample);
        if (frame.w % n != 0 || frame.h % n != 0) {
            std::fprintf(stderr, "vkhost: FAILED: --hash-sample: frame %zu (%ux%u) is not a multiple of %ux\n", m_count,
                         frame.w, frame.h, n);
            m_failed = true;
            return;
        }
        const uint32_t w = frame.w / n;
        const uint32_t h = frame.h / n;
        const std::vector<uint32_t> native = vkhost::SampleImage(frame.xrgb.data(), frame.w, frame.h, n, m_rotation);
        const uint64_t hash = brimir_tools::HashFrame(w, h, native.data(), size_t{w} * 4);
        std::printf("frame %zu %ux%u %016llx\n", m_count, w, h, static_cast<unsigned long long>(hash));
        ++m_sampleFrames;
        m_sampledPoints += vkhost::SampledPointCount(w, h, m_rotation);
        m_samplePoints += uint64_t{w} * h;
    }

    // vkhost::ComputeSharpness (sample_points.hpp) over the region.
    void Sharpness(const vkhost::HostFrame& frame) {
        const uint32_t n = FrameScale(frame, m_sharpness);
        const vkhost::Sharpness s = vkhost::ComputeSharpness(frame.xrgb.data(), frame.w, frame.h, n, m_rotation, m_region);
        if (!s.valid) {
            std::fprintf(stderr, "vkhost: FAILED: --sharpness: frame %zu (%ux%u) is not a multiple of %ux\n", m_count,
                         frame.w, frame.h, n);
            m_failed = true;
            return;
        }
        std::fprintf(stderr,
                     "vkhost: sharpness: frame %zu %ux region %u,%u %ux%u: blocks %llu, non-constant %llu (%.2f%%), mean "
                     "abs diff %.4f\n",
                     m_count, n, s.x0, s.y0, s.w, s.h, static_cast<unsigned long long>(s.blocks),
                     static_cast<unsigned long long>(s.nonConstant), s.nonConstantFraction * 100.0, s.meanAbsDiff);
        ++m_sharpFrames;
        m_sharpScaledFrames += n > 1 ? 1 : 0;
        m_sumNonConstant += s.nonConstantFraction;
        m_sumMeanDiff += s.meanAbsDiff;
    }

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
    int m_dumpEvery = 0;
    size_t m_count = 0;
    bool m_failed = false;
    uint32_t m_hashSample = 0;
    uint32_t m_sharpness = 0;
    uint32_t m_rotation = 0;
    Region m_region;
    size_t m_sampleFrames = 0;
    uint64_t m_sampledPoints = 0;
    uint64_t m_samplePoints = 0;
    size_t m_sharpFrames = 0;
    size_t m_sharpScaledFrames = 0;
    double m_sumNonConstant = 0;
    double m_sumMeanDiff = 0;
};

// --probe summary: one line per measure.
void PrintProbeSummary(const std::vector<vkhost::ProbeFrame>& frames) {
    std::fprintf(stderr, "vkhost: probe: %zu VDP2 frames\n", frames.size());
    if (frames.empty()) {
        return;
    }
    const auto stats = [&](auto member) {
        std::vector<double> values;
        values.reserve(frames.size());
        for (const vkhost::ProbeFrame& f : frames) {
            values.push_back(static_cast<double>(f.*member));
        }
        std::sort(values.begin(), values.end());
        double sum = 0;
        for (double v : values) {
            sum += v;
        }
        struct {
            double median, max, mean;
        } s{values[values.size() / 2], values.back(), sum / static_cast<double>(values.size())};
        return s;
    };
    const auto printStats = [&](const char* name, auto member) {
        const auto s = stats(member);
        std::fprintf(stderr, "vkhost: probe: %-14s median %.1f max %.1f mean %.1f\n", name, s.median, s.max, s.mean);
    };
    printStats("batches", &vkhost::ProbeFrame::batches);
    printStats("arena_bytes", &vkhost::ProbeFrame::arenaBytes);
    printStats("copy_bytes", &vkhost::ProbeFrame::copyBytes);
    printStats("front_us", &vkhost::ProbeFrame::frontMicroseconds);
    printStats("vdp1_front_us", &vkhost::ProbeFrame::vdp1FrontMicroseconds);
    printStats("vdp1_record_us", &vkhost::ProbeFrame::vdp1RecordMicroseconds);
    printStats("vdp1_gpu_us", &vkhost::ProbeFrame::vdp1GpuMicroseconds);
    const auto printFeature = [&](const char* name, uint32_t vkhost::ProbeFrame::*member) {
        size_t usingFrames = 0;
        uint64_t total = 0;
        uint32_t most = 0;
        for (const vkhost::ProbeFrame& f : frames) {
            usingFrames += f.*member != 0;
            total += f.*member;
            most = std::max(most, f.*member);
        }
        std::fprintf(stderr, "vkhost: probe: %-14s frames %zu, total %llu, max per frame %u\n", name, usingFrames,
                     static_cast<unsigned long long>(total), most);
    };
    printFeature("mosaic", &vkhost::ProbeFrame::mosaicLines);
    printFeature("vcell", &vkhost::ProbeFrame::vcellLines);
    printFeature("chardelay", &vkhost::ProbeFrame::charPatDelayLines);
    printFeature("ddlinescroll", &vkhost::ProbeFrame::ddLineScrollLines);
    printFeature("extcc", &vkhost::ProbeFrame::extendedCCLines);
    printFeature("shadow", &vkhost::ProbeFrame::shadowLines);
    printFeature("mesh", &vkhost::ProbeFrame::meshSlices);
    uint32_t cramModes = 0;
    for (const vkhost::ProbeFrame& f : frames) {
        cramModes |= f.cramModeMask;
    }
    std::fprintf(stderr, "vkhost: probe: cram modes used 0x%x\n", cramModes);
}

// --probe summary of the VDP1 counters (cores built with BRIMIR_VDP1_PROBE=ON): one line per counter.
void PrintVdp1ProbeSummary(const std::vector<vkhost::Vdp1ProbeFrame>& frames) {
    std::fprintf(stderr, "vkhost: probe-vdp1: %zu frames\n", frames.size());
    if (frames.empty()) {
        return;
    }
    const auto printValues = [&](const char* name, const auto& get) {
        std::vector<uint64_t> values;
        values.reserve(frames.size());
        uint64_t sum = 0;
        size_t usingFrames = 0;
        for (const vkhost::Vdp1ProbeFrame& f : frames) {
            const uint64_t v = get(f);
            values.push_back(v);
            sum += v;
            usingFrames += v != 0;
        }
        std::sort(values.begin(), values.end());
        std::fprintf(stderr, "vkhost: probe-vdp1: %-10s median %llu max %llu mean %.1f frames %zu total %llu\n", name,
                     static_cast<unsigned long long>(values[values.size() / 2]),
                     static_cast<unsigned long long>(values.back()),
                     static_cast<double>(sum) / static_cast<double>(values.size()), usingFrames,
                     static_cast<unsigned long long>(sum));
    };
    const auto printStats = [&](const char* name, uint64_t vkhost::Vdp1ProbeFrame::*member) {
        printValues(name, [member](const vkhost::Vdp1ProbeFrame& f) { return f.*member; });
    };
    printStats("commands", &vkhost::Vdp1ProbeFrame::commands);
    printStats("lines", &vkhost::Vdp1ProbeFrame::lines);
    printStats("pixels", &vkhost::Vdp1ProbeFrame::pixels);
    printStats("syncs", &vkhost::Vdp1ProbeFrame::syncs);
    printStats("dirtysyncs", &vkhost::Vdp1ProbeFrame::dirtySyncs);
    printStats("fbwrites", &vkhost::Vdp1ProbeFrame::fbWrites);
    printStats("erases", &vkhost::Vdp1ProbeFrame::erases);
    // GPU VDP1 front half: draw batches per retro_run, in all and by end reason
    printValues("batches", [](const vkhost::Vdp1ProbeFrame& f) {
        uint64_t n = 0;
        for (uint64_t b : f.batchEnds) {
            n += b;
        }
        return n;
    });
    static constexpr const char* kEnds[6] = {"b_opkind", "b_hazard", "b_swap", "b_capacity", "b_boundary", "b_take"};
    for (int i = 0; i < 6; ++i) {
        printValues(kEnds[i], [i](const vkhost::Vdp1ProbeFrame& f) { return f.batchEnds[i]; });
    }
}

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

    if (!args.fixedDate.empty()) {
        // Read by the core in retro_load_game; inherited by the --compare children (which also get the flag).
#ifdef _WIN32
        _putenv_s("BRIMIR_RTC_FIXED", args.fixedDate.c_str());
#else
        setenv("BRIMIR_RTC_FIXED", args.fixedDate.c_str(), 1);
#endif
    }

    if (args.compare) {
        vkhost::CompareConfig compare;
        compare.args.assign(argv + 1, argv + argc);
        compare.systemDir = args.config.systemDir;
        compare.dumpDir = args.config.dumpDir;
        compare.frames = args.config.frames;
        compare.vulkanOptions = args.compareVulkanOptions;
        compare.stateEvery = args.compareState;
        compare.strictStates = !args.fixedDate.empty();
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
    sink.SetDumpEvery(args.dumpEvery);
    sink.SetSampling(static_cast<uint32_t>(args.hashSample), static_cast<uint32_t>(args.sharpness), args.rotation,
                     args.region);
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
                     "hidden-frame GPU calls %llu, validation %s, validation errors %u, hidden-frame submits %llu, "
                     "max submits per retro_run %llu\n",
                     static_cast<unsigned long long>(report.contextResets),
                     static_cast<unsigned long long>(report.coreSubmits),
                     static_cast<unsigned long long>(report.lockViolations),
                     static_cast<unsigned long long>(report.hiddenGpuCalls), report.validation ? "on" : "off",
                     report.validationErrors, static_cast<unsigned long long>(report.hiddenSubmits),
                     static_cast<unsigned long long>(report.maxRunSubmits));
        const vkhost::SubmitWaitStats& s = report.submitWaits;
        const auto u = [](uint64_t n) { return static_cast<unsigned long long>(n); };
        std::fprintf(stderr,
                     "vkhost: vulkan: submits: sync %llu, context %llu, refused %llu, ops-only after refused %llu, "
                     "outside %llu; waits: frame %llu, sync %llu, serialize %llu, context %llu, outside the allowed "
                     "list %llu\n",
                     u(s.syncSubmits), u(s.contextSubmits), u(s.refusedSubmits), u(s.extraOpsOnlyRuns),
                     u(s.outsideSubmits), u(s.frameWaits), u(s.syncWaits), u(s.serializeWaits), u(s.contextWaits),
                     u(s.outsideWaits + s.outsideSubmits));
    }
    if (report.serializeCalls > 0) {
        std::fprintf(stderr,
                     "vkhost: state: %llu retro_serialize calls, %.3f ms mean, %.3f ms max; retro_unserialize %.3f ms "
                     "total\n",
                     static_cast<unsigned long long>(report.serializeCalls),
                     report.serializeSeconds * 1e3 / static_cast<double>(report.serializeCalls),
                     report.maxSerializeSeconds * 1e3, report.unserializeSeconds * 1e3);
    }
    std::fprintf(stderr, "vkhost: av: SET_SYSTEM_AV_INFO calls %llu, SET_GEOMETRY calls %llu\n",
                 static_cast<unsigned long long>(report.systemAvInfoCalls),
                 static_cast<unsigned long long>(report.geometryCalls));
    sink.PrintSampleSummary();
    sink.PrintSharpnessSummary();
    if (args.config.probe) {
        if (args.config.vulkan) {
            PrintProbeSummary(report.probeFrames);
        }
        PrintVdp1ProbeSummary(report.vdp1ProbeFrames);
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
