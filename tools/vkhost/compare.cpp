// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "compare.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
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
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace vkhost {

namespace {

namespace fs = std::filesystem;

// Command line flags that take no value; every other flag consumes the next argument.
const std::set<std::string> kValueless = {"--hash",        "--verbose", "--vulkan",  "--refuse-hw",
                                          "--runahead",    "--compare", "--fail-compute-on-grow", "--fail-compute-on-reset", "--no-validation"};
// Dropped from both children (the compare adds its own).
const std::set<std::string> kDropped = {"--compare", "--hash",   "--vulkan",       "--dump-dir", "--dump-frame",
                                        "--system-dir", "--frames", "--compare-vulkan-option"};
// Only valid together with --vulkan: dropped from the software child.
const std::set<std::string> kVulkanOnly = {"--reset-at",   "--reset-in-run", "--reset-in-present", "--preferred-hw",
                                           "--fake-api",   "--grow-mask",    "--fail-compute-on-grow", "--fail-compute-on-reset", "--no-validation",
                                           "--sharpness",  "--region",       "--fail-alloc-above"};
// Takes a frame number and a file; each child writes its own file (<file>.software, <file>.vulkan).
const std::string kSaveStateAt = "--save-state-at";
// Takes a frame number and key=value; both children get it.
const std::string kSetOptionAt = "--set-option-at";
// The software child samples its 1x frames (the same excluded lattice); the Vulkan child the given N.
const std::string kHashSample = "--hash-sample";

fs::path SelfPath() {
#ifdef _WIN32
    char buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (n > 0 && n < sizeof(buffer)) {
        return fs::path(std::string(buffer, n));
    }
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return exe;
    }
#endif
    return {};
}

std::string Quote(const std::string& text) {
#ifdef _WIN32
    return "\"" + text + "\"";
#else
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out + "'";
#endif
}

unsigned long ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

struct ChildResult {
    int exitCode = -1;
    double seconds = 0;
    std::vector<std::string> hashLines; // "frame <i> <w>x<h> <hash>"
    fs::path stderrFile;
};

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Runs this executable once. A system directory is copied first, because the core rewrites the RTC file in it.
ChildResult RunChild(const fs::path& self, const std::vector<std::string>& args, const std::string& systemDir,
                     const fs::path& workDir, const std::string& tag) {
    ChildResult result;
    std::vector<std::string> full = args;
    fs::path sysCopy;
    if (!systemDir.empty()) {
        sysCopy = workDir / ("system-" + tag);
        std::error_code ec;
        fs::remove_all(sysCopy, ec);
        fs::copy(systemDir, sysCopy, fs::copy_options::recursive, ec);
        if (ec) {
            std::fprintf(stderr, "compare: cannot copy the system directory: %s\n", ec.message().c_str());
            return result;
        }
        full.push_back("--system-dir");
        full.push_back(sysCopy.string());
    }
    const fs::path outFile = workDir / ("stdout-" + tag + ".txt");
    result.stderrFile = workDir / ("stderr-" + tag + ".txt");

    std::string command = Quote(self.string());
    for (const std::string& arg : full) {
        command += " " + Quote(arg);
    }
    command += " > " + Quote(outFile.string()) + " 2> " + Quote(result.stderrFile.string());
#ifdef _WIN32
    command = "\"" + command + "\""; // cmd /c strips one pair of outer quotes
#endif
    const auto start = std::chrono::steady_clock::now();
    const int status = std::system(command.c_str());
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
#ifdef _WIN32
    result.exitCode = status;
#else
    result.exitCode = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
#endif
    if (!sysCopy.empty()) {
        std::error_code ec;
        fs::remove_all(sysCopy, ec);
    }

    std::istringstream lines(ReadAll(outFile));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("frame ", 0) == 0) {
            result.hashLines.push_back(line);
        }
    }
    return result;
}

// The lines of a child's stderr that say how the run went (its own summary and any check failure).
void PrintSummary(const char* label, const ChildResult& child) {
    std::istringstream lines(ReadAll(child.stderrFile));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("vkhost: ", 0) == 0) {
            std::printf("compare: %s: %s\n", label, line.c_str() + 8);
        }
    }
}

struct Ppm {
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> rgb;
};

bool ReadPpm(const fs::path& path, Ppm& out) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    unsigned w = 0;
    unsigned h = 0;
    unsigned maxval = 0;
    in >> magic >> w >> h >> maxval;
    if (!in || magic != "P6" || maxval != 255 || w == 0 || h == 0) {
        return false;
    }
    in.get(); // the single whitespace after the header
    out.w = w;
    out.h = h;
    out.rgb.resize(static_cast<size_t>(w) * h * 3);
    in.read(reinterpret_cast<char*>(out.rgb.data()), static_cast<std::streamsize>(out.rgb.size()));
    return static_cast<bool>(in);
}

std::vector<std::string> BuildArgs(const std::vector<std::string>& in, bool vulkan) {
    std::vector<std::string> out;
    for (size_t i = 0; i < in.size(); ++i) {
        const std::string& arg = in[i];
        if (arg == kSaveStateAt && i + 2 < in.size()) {
            out.push_back(arg);
            out.push_back(in[i + 1]);
            out.push_back(in[i + 2] + (vulkan ? ".vulkan" : ".software"));
            i += 2;
            continue;
        }
        if (arg == kSetOptionAt && i + 2 < in.size()) {
            out.insert(out.end(), {arg, in[i + 1], in[i + 2]});
            i += 2;
            continue;
        }
        if (arg == kHashSample && i + 1 < in.size()) {
            out.insert(out.end(), {arg, vulkan ? in[i + 1] : std::string("1")});
            i += 1;
            continue;
        }
        const bool valueless = kValueless.count(arg) != 0;
        const bool drop = kDropped.count(arg) != 0 || (!vulkan && kVulkanOnly.count(arg) != 0);
        if (!drop) {
            out.push_back(arg);
        }
        if (!valueless && i + 1 < in.size()) {
            ++i;
            if (!drop) {
                out.push_back(in[i]);
            }
        }
    }
    if (vulkan) {
        out.push_back("--vulkan");
    }
    return out;
}

// Removes the work directory when it goes out of scope unless the logs and dumps are worth keeping.
class WorkDir {
public:
    explicit WorkDir(fs::path path) : m_path(std::move(path)) {}
    ~WorkDir() {
        if (!m_keep) {
            std::error_code ec;
            fs::remove_all(m_path, ec);
        }
    }
    WorkDir(const WorkDir&) = delete;
    WorkDir& operator=(const WorkDir&) = delete;
    const fs::path& Path() const { return m_path; }
    void Keep() { m_keep = true; }

private:
    fs::path m_path;
    bool m_keep = false;
};

// What the Vulkan child's stderr says about how it ran.
struct VulkanFacts {
    bool outputReady = false;                    // "Vulkan frame output ready"
    bool rendererActive = false;                 // "Vulkan VDP2 renderer active"
    std::vector<std::string> degraded;           // log lines that mean software or copy-only output
    long long hardwareFrames = -1;               // "recorded N frames (H hardware)"
    long long submits = -1;                      // "core submits S"
    bool validationOn = false;
    long long validationErrors = -1;
};

VulkanFacts ParseVulkanFacts(const fs::path& stderrFile) {
    static const char* const kDegraded[] = {"falling back to software",
                                            "frontend video driver is not Vulkan",
                                            "Vulkan context unusable",
                                            "using copy-only frame output",
                                            "Vulkan frame output changed to",
                                            "Vulkan frame output failed",
                                            "Vulkan context still live",
                                            "Vulkan VDP2 renderer unavailable",
                                            "Vulkan VDP2 renderer off"};
    VulkanFacts facts;
    std::istringstream lines(ReadAll(stderrFile));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find("Vulkan frame output ready") != std::string::npos) {
            facts.outputReady = true;
        }
        if (line.find("Vulkan VDP2 renderer active") != std::string::npos) {
            facts.rendererActive = true;
        }
        for (const char* needle : kDegraded) {
            if (line.find(needle) != std::string::npos) {
                facts.degraded.push_back(line);
                break;
            }
        }
        long long frames = 0;
        long long hardware = 0;
        if (std::sscanf(line.c_str(), "vkhost: recorded %lld frames (%lld hardware)", &frames, &hardware) == 2) {
            facts.hardwareFrames = hardware;
        }
        long long resets = 0;
        long long submits = 0;
        if (std::sscanf(line.c_str(), "vkhost: vulkan: context resets %lld, core submits %lld,", &resets, &submits) == 2) {
            facts.submits = submits;
            facts.validationOn = line.find("validation on,") != std::string::npos;
            const size_t pos = line.find("validation errors ");
            if (pos != std::string::npos) {
                facts.validationErrors = std::atoll(line.c_str() + pos + 18);
            }
        }
    }
    return facts;
}

// The last lines of a child's stderr, for a run that failed before it produced anything to compare.
void PrintTail(const char* label, const ChildResult& child) {
    std::vector<std::string> all;
    std::istringstream lines(ReadAll(child.stderrFile));
    std::string line;
    while (std::getline(lines, line)) {
        all.push_back(line);
    }
    const size_t from = all.size() > 12 ? all.size() - 12 : 0;
    for (size_t i = from; i < all.size(); ++i) {
        std::fprintf(stderr, "compare: %s stderr: %s\n", label, all[i].c_str());
    }
}

} // namespace

int RunCompare(const CompareConfig& config) {
    const fs::path self = SelfPath();
    if (self.empty()) {
        std::fprintf(stderr, "compare: cannot find this executable\n");
        return 2;
    }
    const fs::path workPath = fs::temp_directory_path() / "brimir_vkhost" /
                              ("compare-" + std::to_string(ProcessId()) + "-" +
                               std::to_string(static_cast<long long>(
                                   std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code ec;
    fs::create_directories(workPath, ec);
    if (ec) {
        std::fprintf(stderr, "compare: cannot create %s: %s\n", workPath.string().c_str(), ec.message().c_str());
        return 2;
    }
    WorkDir workGuard(workPath);
    const fs::path& work = workGuard.Path();
    const fs::path dumpRoot = config.dumpDir.empty() ? work / "dump" : fs::path(config.dumpDir);

    std::vector<std::string> swArgs = BuildArgs(config.args, false);
    std::vector<std::string> vkArgs = BuildArgs(config.args, true);
    for (const auto& [key, value] : config.vulkanOptions) {
        vkArgs.push_back("--option");
        vkArgs.push_back(key + "=" + value);
    }
    // Test hook (tests/vkhost_checks.cmake): extra arguments for the Vulkan child only, so the "the Vulkan run really
    // used the compute output" checks below can be exercised with --refuse-hw, --fake-api and the like.
    if (const char* extra = std::getenv("BRIMIR_VKHOST_COMPARE_VULKAN_ARGS")) {
        std::istringstream words(extra);
        std::string word;
        while (words >> word) {
            vkArgs.push_back(word);
        }
    }
    const std::vector<std::string> frameArgs = {"--frames", std::to_string(config.frames), "--hash"};
    swArgs.insert(swArgs.end(), frameArgs.begin(), frameArgs.end());
    vkArgs.insert(vkArgs.end(), frameArgs.begin(), frameArgs.end());

    std::printf("compare: software run (%d frames)...\n", config.frames);
    std::fflush(stdout);
    const ChildResult sw = RunChild(self, swArgs, config.systemDir, work, "sw");
    PrintSummary("software", sw);
    std::printf("compare: software: exit %d, %zu frames, %.1f s\n", sw.exitCode, sw.hashLines.size(), sw.seconds);
    if (sw.exitCode != 0) {
        std::fprintf(stderr, "compare: the software run failed\n");
        PrintTail("software", sw);
        return 2;
    }

    std::printf("compare: vulkan run...\n");
    std::fflush(stdout);
    const ChildResult vk = RunChild(self, vkArgs, config.systemDir, work, "vk");
    PrintSummary("vulkan", vk);
    std::printf("compare: vulkan: exit %d, %zu frames, %.1f s\n", vk.exitCode, vk.hashLines.size(), vk.seconds);
    if (vk.exitCode == 4) {
        std::fprintf(stderr, "compare: no usable Vulkan device\n");
        return 4;
    }
    if (vk.exitCode != 0 && vk.exitCode != 3) {
        std::fprintf(stderr, "compare: the vulkan run failed\n");
        PrintTail("vulkan", vk);
        return 2;
    }

    const size_t common = std::min(sw.hashLines.size(), vk.hashLines.size());
    size_t differing = 0;
    size_t first = common;
    for (size_t i = 0; i < common; ++i) {
        if (sw.hashLines[i] != vk.hashLines[i]) {
            if (first == common) {
                first = i;
            }
            ++differing;
        }
    }
    const size_t expected = static_cast<size_t>(config.frames);
    bool failed = false;
    auto fail = [&](const std::string& message) {
        failed = true;
        std::printf("compare: FAILED: %s\n", message.c_str());
    };
    if (vk.exitCode != 0) {
        fail("the vulkan run's own checks failed (exit 3)");
    }
    if (sw.hashLines.size() != vk.hashLines.size()) {
        fail("frame counts differ: software " + std::to_string(sw.hashLines.size()) + ", vulkan " +
             std::to_string(vk.hashLines.size()));
    }
    if (sw.hashLines.size() != expected || vk.hashLines.size() != expected) {
        fail("expected " + std::to_string(expected) + " frames from each run, got software " +
             std::to_string(sw.hashLines.size()) + ", vulkan " + std::to_string(vk.hashLines.size()));
    }

    // The Vulkan run must have used the compute output for every frame, or the comparison proves nothing.
    const VulkanFacts facts = ParseVulkanFacts(vk.stderrFile);
    if (!facts.outputReady) {
        fail("no \"Vulkan frame output ready\" in the vulkan run's log: the core did not take the Vulkan path");
    }
    if (!facts.rendererActive) {
        fail("no \"Vulkan VDP2 renderer active\" in the vulkan run's log: VDP2 did not run on the GPU");
    }
    for (const std::string& line : facts.degraded) {
        fail("the vulkan run was not on the compute output: " + line);
    }
    if (facts.hardwareFrames != static_cast<long long>(expected)) {
        fail("the vulkan run read back " + std::to_string(facts.hardwareFrames) + " hardware frames, expected " +
             std::to_string(expected));
    }
    if (facts.submits <= 0) {
        fail("the vulkan run made " + std::to_string(facts.submits) + " core submits");
    }

    if (differing > 0) {
        failed = true;
        std::printf("compare: FAILED: %zu of %zu frames differ, first at frame %zu\n", differing, common, first);
        std::printf("compare:   software: %s\n", sw.hashLines[first].c_str());
        std::printf("compare:   vulkan  : %s\n", vk.hashLines[first].c_str());

        // Run both again up to that frame and dump just it.
        const fs::path swDump = dumpRoot / "software";
        const fs::path vkDump = dumpRoot / "vulkan";
        fs::create_directories(swDump, ec);
        fs::create_directories(vkDump, ec);
        const std::string frameCount = std::to_string(first + 1);
        auto rerun = [&](bool vulkan, const fs::path& dir, const std::string& tag) {
            std::vector<std::string> args = BuildArgs(config.args, vulkan);
            if (vulkan) {
                for (const auto& [key, value] : config.vulkanOptions) {
                    args.push_back("--option");
                    args.push_back(key + "=" + value);
                }
            }
            for (const std::string& a : {std::string("--frames"), frameCount, std::string("--hash"),
                                         std::string("--dump-dir"), dir.string(), std::string("--dump-frame"),
                                         std::to_string(first)}) {
                args.push_back(a);
            }
            return RunChild(self, args, config.systemDir, work, tag);
        };
        const ChildResult swAgain = rerun(false, swDump, "sw-dump");
        const ChildResult vkAgain = rerun(true, vkDump, "vk-dump");
        const bool reproduced = swAgain.hashLines.size() == first + 1 && vkAgain.hashLines.size() == first + 1 &&
                                swAgain.hashLines[first] == sw.hashLines[first] &&
                                vkAgain.hashLines[first] == vk.hashLines[first];
        if (!reproduced) {
            std::printf("compare:   note: the re-run did not reproduce the same frame hashes (non-deterministic?)\n");
        }

        char name[32];
        std::snprintf(name, sizeof(name), "frame_%05zu.ppm", first);
        Ppm a;
        Ppm b;
        const bool haveA = ReadPpm(swDump / name, a);
        const bool haveB = ReadPpm(vkDump / name, b);
        if (!haveA || !haveB) {
            std::printf("compare:   could not read the dumped frames in %s\n", dumpRoot.string().c_str());
        } else {
            std::printf("compare:   frame %zu: software %ux%u, vulkan %ux%u; dumps: %s, %s\n", first, a.w, a.h, b.w,
                        b.h, (swDump / name).string().c_str(), (vkDump / name).string().c_str());
            if (a.w == b.w && a.h == b.h) {
                size_t pixels = 0;
                bool reported = false;
                for (uint32_t y = 0; y < a.h; ++y) {
                    for (uint32_t x = 0; x < a.w; ++x) {
                        const size_t o = (static_cast<size_t>(y) * a.w + x) * 3;
                        if (!std::equal(a.rgb.begin() + o, a.rgb.begin() + o + 3, b.rgb.begin() + o)) {
                            ++pixels;
                            if (!reported) {
                                reported = true;
                                std::printf("compare:   first differing pixel (%u,%u): software 0x%02X%02X%02X, "
                                            "vulkan 0x%02X%02X%02X\n",
                                            x, y, a.rgb[o], a.rgb[o + 1], a.rgb[o + 2], b.rgb[o], b.rgb[o + 1],
                                            b.rgb[o + 2]);
                            }
                        }
                    }
                }
                if (pixels == 0) {
                    std::printf("compare:   the RGB pixels are identical (the hash covers the X byte too)\n");
                } else {
                    std::printf("compare:   %zu of %zu pixels differ\n", pixels, static_cast<size_t>(a.w) * a.h);
                }
            }
        }
    }

    if (failed) {
        workGuard.Keep();
        std::printf("compare: FAILED (work directory kept: %s)\n", work.string().c_str());
        return 3;
    }
    std::printf("compare: OK, %zu frames identical (software vs vulkan VDP2 renderer: %lld hardware frames, "
                "%lld core submits, validation %s, %lld validation errors)\n",
                common, facts.hardwareFrames, facts.submits, facts.validationOn ? "on" : "off",
                facts.validationErrors);
    return 0;
}

} // namespace vkhost