# SH-2 JIT Milestone 1A — Measurement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure how much host time the two SH-2 CPUs take per frame, with a headless benchmark tool, and record a baseline before any JIT work.

**Architecture:** The SH-2 files become Brimir-owned (fork in place, logged in `src/core/BRIMIR_FORK.md`). `SH2::Advance` gains optional host-time accounting behind a runtime flag. `CoreWrapper` feeds the per-frame SH-2 times into the existing `Profiler`. A new `tools/brimir_bench` runs real content headlessly and reports ms/frame and component shares. It replaces the `benchmark_sh2` micro-benchmark.

**Tech Stack:** C++20, CMake 3.28+, Catch2 (amalgamated, `tests/`), MSVC 2022 / GCC 14 / Apple Clang.

**Spec:** `design/sh2-jit.md` sections 3 (ownership) and 8 (measurement).

## Global Constraints

- Fork scope is exactly `src/core/include/ymir/hw/sh2/*` and `src/core/src/ymir/hw/sh2/*`. Do not edit any other file under `src/core/`.
- Every Brimir change inside the fork is marked with a `// Brimir:` comment and logged in `src/core/BRIMIR_FORK.md`.
- With profiling disabled, emulation behavior must be byte-identical to today; the only added cost is one `bool` check per `SH2::Advance` call.
- Tests: Catch2 in `tests/unit/`, registered in `tests/CMakeLists.txt`. Scratch files go to a temp directory, never the source tree.
- Commit messages follow the repo style: `type(scope): subject`.

## Build and test commands (Windows)

The MSVC environment must be loaded. From a *Developer PowerShell for VS 2022*:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBRIMIR_BUILD_TESTS=ON
cmake --build build --target brimir_tests brimir_libretro
ctest --test-dir build --output-on-failure
build\bin\brimir_tests.exe "[profiler]"
```

Linux (GCC 14): same commands with `-DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_C_COMPILER=gcc-14` and `./build/bin/brimir_tests`.

---

### Task 1: Fork bookkeeping and SH-2 host-time accounting

**Files:**
- Create: `src/core/BRIMIR_FORK.md`
- Modify: `src/core/include/ymir/hw/sh2/sh2.hpp` (public section after `Step()` declaration at line 98; private cycle-counting section near line 704)
- Modify: `src/core/src/ymir/hw/sh2/sh2.cpp` (includes at top; `SH2::Advance` at line 435)
- Modify: `include/brimir/profiler.hpp` (public section of `Profiler`)
- Modify: `include/brimir/core_wrapper.hpp` (public section, next to `GetProfilingReport`)
- Modify: `src/bridge/core_wrapper.cpp` (`Initialize` ~line 147, `SetProfilingEnabled` ~line 936, `RunFrame` ~line 940)
- Modify: `ROADMAP.md` ("Guiding Principle" section at the top)
- Modify: `README.md` ("Project Structure" block and the Credits paragraph)
- Test: `tests/unit/test_profiler.cpp`

**Interfaces:**
- Produces (fork, `ymir::sh2::SH2`, public):
  - `void SetHostTimeProfiling(bool enable)` — enables/disables accounting and clears the counter
  - `uint64 ConsumeHostTimeNs()` — returns nanoseconds spent in `Advance()` since the last call and resets the counter
- Produces (`brimir::Profiler`, public):
  - `bool IsEnabled() const`
  - `void AddSample(const std::string& name, double ms)` — one sample, ignored while disabled
- Produces (`brimir::CoreWrapper`, public):
  - `const Profiler& GetProfiler() const`
  - Profiler sample names written once per `RunFrame` while profiling is enabled: `"SH2_Master"`, `"SH2_Slave"` (existing: `"RunFrame_Total"`, `"Ymir_RunFrame"`)

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_profiler.cpp`:

```cpp
TEST_CASE("Profiler AddSample aggregates only while enabled", "[profiler][unit]") {
    brimir::Profiler profiler;
    REQUIRE_FALSE(profiler.IsEnabled());
    profiler.AddSample("X", 1.0);
    REQUIRE_FALSE(profiler.GetTiming("X").has_value());

    profiler.SetEnabled(true);
    REQUIRE(profiler.IsEnabled());
    profiler.AddSample("X", 1.0);
    profiler.AddSample("X", 3.0);

    const auto timing = profiler.GetTiming("X");
    REQUIRE(timing.has_value());
    REQUIRE(timing->count == 2);
    REQUIRE(timing->totalMs == Catch::Approx(4.0));
    REQUIRE(timing->minMs == Catch::Approx(1.0));
    REQUIRE(timing->maxMs == Catch::Approx(3.0));
}

TEST_CASE("SH-2 host time accounting is off by default and consumable", "[profiler][sh2]") {
    brimir::CoreWrapper core;
    REQUIRE(core.Initialize());
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);

    // Uses the built-in null IPL program (no BIOS loaded).
    core.RunFrame();
    REQUIRE(saturn->masterSH2.ConsumeHostTimeNs() == 0);

    saturn->masterSH2.SetHostTimeProfiling(true);
    core.RunFrame();
    REQUIRE(saturn->masterSH2.ConsumeHostTimeNs() > 0);
    REQUIRE(saturn->masterSH2.ConsumeHostTimeNs() == 0);

    saturn->masterSH2.SetHostTimeProfiling(false);
    core.RunFrame();
    REQUIRE(saturn->masterSH2.ConsumeHostTimeNs() == 0);
}

TEST_CASE("Enabled wrapper profiling records SH-2 time per frame", "[profiler][integration]") {
    brimir::CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetProfilingEnabled(true);
    for (int i = 0; i < 3; ++i) {
        core.RunFrame();
    }

    const auto master = core.GetProfiler().GetTiming("SH2_Master");
    const auto slave = core.GetProfiler().GetTiming("SH2_Slave");
    REQUIRE(master.has_value());
    REQUIRE(slave.has_value());
    REQUIRE(master->count == 3);
    REQUIRE(slave->count == 3);
    REQUIRE(master->totalMs > 0.0);
}

TEST_CASE("Profiling enabled before Initialize applies to the SH-2s", "[profiler][integration]") {
    brimir::CoreWrapper core;
    core.SetProfilingEnabled(true);
    REQUIRE(core.Initialize());
    core.RunFrame();
    const auto master = core.GetProfiler().GetTiming("SH2_Master");
    REQUIRE(master.has_value());
    REQUIRE(master->totalMs > 0.0);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target brimir_tests`
Expected: compile errors — `IsEnabled`, `AddSample`, `SetHostTimeProfiling`, `ConsumeHostTimeNs`, `GetProfiler` are not members.

- [ ] **Step 3: Add host-time accounting to the fork**

In `src/core/include/ymir/hw/sh2/sh2.hpp`, directly after the `Step()` declaration (line 98), add:

```cpp
    // -------------------------------------------------------------------------
    // Brimir: host-time profiling (see src/core/BRIMIR_FORK.md)

    /// @brief Enables or disables host wall-time accounting for Advance(). Always clears the counter.
    void SetHostTimeProfiling(bool enable) {
        m_profileHostTime = enable;
        m_hostTimeNs = 0;
    }

    /// @brief Returns the host time spent in Advance() since the last call, in nanoseconds, and resets it.
    uint64 ConsumeHostTimeNs() {
        const uint64 ns = m_hostTimeNs;
        m_hostTimeNs = 0;
        return ns;
    }
```

In the same file, directly after `uint64 m_cyclesExecuted;` (line 704), add:

```cpp

    // Brimir: host-time profiling state (see SetHostTimeProfiling)
    bool m_profileHostTime = false;
    uint64 m_hostTimeNs = 0;
```

In `src/core/src/ymir/hw/sh2/sh2.cpp`, add `#include <chrono>` after `#include <cassert>` (line 10). Then, directly before the `template <bool debug, bool emulateCache>` line that precedes `FLATTEN uint64 SH2::Advance` (line 434), add:

```cpp
namespace {

// Brimir: adds the host wall time of a scope to a counter when enabled.
struct HostTimeScope {
    HostTimeScope(bool enabled, uint64 &counter)
        : m_enabled(enabled)
        , m_counter(counter) {
        if (m_enabled) {
            m_start = std::chrono::steady_clock::now();
        }
    }

    ~HostTimeScope() {
        if (m_enabled) {
            const auto elapsed = std::chrono::steady_clock::now() - m_start;
            m_counter += static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        }
    }

    HostTimeScope(const HostTimeScope &) = delete;
    HostTimeScope &operator=(const HostTimeScope &) = delete;

    bool m_enabled;
    uint64 &m_counter;
    std::chrono::steady_clock::time_point m_start{};
};

} // namespace

```

Make the first statement of `SH2::Advance` (before `m_cyclesExecuted = spilloverCycles;`):

```cpp
    HostTimeScope hostTime{m_profileHostTime, m_hostTimeNs}; // Brimir: host-time profiling
```

- [ ] **Step 4: Add `IsEnabled` and `AddSample` to the profiler**

In `include/brimir/profiler.hpp`, inside `class Profiler`'s public section, directly after `SetEnabled(...)`, add:

```cpp
    /// @brief Whether samples are currently being collected.
    bool IsEnabled() const {
        return (m_state.load(std::memory_order_relaxed) & 1) != 0;
    }

    /// @brief Record an externally measured duration as one sample. Ignored while disabled.
    void AddSample(const std::string& name, double ms) {
        std::lock_guard lock(m_mutex);
        if (!(m_state.load(std::memory_order_relaxed) & 1)) {
            return;
        }
        auto& timing = m_timings[name];
        timing.totalMs += ms;
        timing.count++;
        timing.minMs = std::min(timing.minMs, ms);
        timing.maxMs = std::max(timing.maxMs, ms);
    }
```

- [ ] **Step 5: Wire the SH-2 times into `CoreWrapper`**

In `include/brimir/core_wrapper.hpp`, directly after `void ResetProfiling() { m_profiler.Reset(); }`, add:

```cpp

    /// @brief Read-only access to the profiler (for tools and tests)
    const Profiler& GetProfiler() const { return m_profiler; }
```

In `src/bridge/core_wrapper.cpp`, replace `SetProfilingEnabled` with:

```cpp
void CoreWrapper::SetProfilingEnabled(bool enabled) {
    m_profiler.SetEnabled(enabled);
    if (m_saturn) {
        m_saturn->masterSH2.SetHostTimeProfiling(enabled);
        m_saturn->slaveSH2.SetHostTimeProfiling(enabled);
    }
}
```

In `CoreWrapper::Initialize`, directly after `m_saturn = std::make_unique<ymir::Saturn>();`, add:

```cpp
        // Apply a profiling state chosen before initialization
        m_saturn->masterSH2.SetHostTimeProfiling(m_profiler.IsEnabled());
        m_saturn->slaveSH2.SetHostTimeProfiling(m_profiler.IsEnabled());
```

In `CoreWrapper::RunFrame`, directly after the closing brace of the `ScopedTimer ymirTimer(m_profiler, "Ymir_RunFrame");` block, add:

```cpp

        // Per-frame SH-2 host time, accumulated inside SH2::Advance
        if (m_profiler.IsEnabled()) {
            m_profiler.AddSample("SH2_Master", static_cast<double>(m_saturn->masterSH2.ConsumeHostTimeNs()) / 1e6);
            m_profiler.AddSample("SH2_Slave", static_cast<double>(m_saturn->slaveSH2.ConsumeHostTimeNs()) / 1e6);
        }
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "[profiler]"`
Expected: `All tests passed` (the four new cases plus the existing profiler cases).

Then the full suite: `ctest --test-dir build --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 7: Write the fork log and update the policy docs**

Create `src/core/BRIMIR_FORK.md`:

```markdown
# Brimir fork of the Ymir SH-2

The Ymir hardware layer under `src/core/` is a verbatim copy of upstream
[Ymir](https://github.com/StrikerX3/ymir), with one exception: the SH-2 CPU is
Brimir-owned so the SH-2 JIT (see `design/sh2-jit.md`) can hook into it.

## Fork scope

- `src/core/include/ymir/hw/sh2/*`
- `src/core/src/ymir/hw/sh2/*`

Everything else under `src/core/` stays verbatim upstream.

## Rules

- Keep Brimir changes minimal and mark each one with a `// Brimir:` comment.
- Log every Brimir change and every upstream SH-2 port in the tables below.
- On an upstream Ymir sync, do not copy the fork-scope files wholesale. Review
  upstream SH-2 commits since the last port and apply them by hand.

## Brimir changes

| Date | Files | Change |
|---|---|---|
| 2026-09-30 | `sh2.hpp`, `sh2.cpp` | Optional host-time accounting in `SH2::Advance` (`SetHostTimeProfiling`, `ConsumeHostTimeNs`) for the profiler and `brimir_bench`. |

## Upstream SH-2 ports

Fork base: upstream Ymir hardware-layer sync of 2026-06-23 plus the backports
listed in `CHANGELOG.md` up to v0.5.4.

| Date | Upstream commit | Files | Notes |
|---|---|---|---|
```

In `ROADMAP.md`, replace the paragraph under "## Guiding Principle" with:

```markdown
**Ymir hardware layer stays verbatim upstream, except the SH-2.** The Ymir source tree under `src/core/` is a direct copy of upstream with one exception: the SH-2 CPU (`src/core/include/ymir/hw/sh2/*`, `src/core/src/ymir/hw/sh2/*`) is Brimir-owned so the SH-2 JIT can hook into it. Changes and hand-ported upstream fixes are logged in [`src/core/BRIMIR_FORK.md`](src/core/BRIMIR_FORK.md). All other Brimir features must be implemented in the Bridge layer (`src/bridge/`), the Libretro layer (`src/libretro/`), or proposed upstream to Ymir.
```

In `README.md`, in the "Project Structure" block, replace the three `src/core/` lines with:

```
    core/
      include/ymir/   Ymir hardware layer (verbatim upstream sync, except the SH-2 fork)
      include/brimir/ Brimir-specific additions
      src/ymir/       Ymir source files (verbatim, except the SH-2 fork)
```

and in the "Credits" paragraph replace the sentence "The entire hardware layer under `src/core/` is synced verbatim from upstream Ymir — all Saturn CPU, VDP, audio, and peripheral emulation is Ymir's work." with:

```
The hardware layer under `src/core/` is synced from upstream Ymir — all Saturn CPU, VDP, audio, and peripheral emulation is Ymir's work. The SH-2 files are a Brimir-maintained fork of Ymir's SH-2 for the upcoming JIT; see `src/core/BRIMIR_FORK.md`.
```

- [ ] **Step 8: Commit**

```bash
git add src/core/BRIMIR_FORK.md src/core/include/ymir/hw/sh2/sh2.hpp src/core/src/ymir/hw/sh2/sh2.cpp include/brimir/profiler.hpp include/brimir/core_wrapper.hpp src/bridge/core_wrapper.cpp tests/unit/test_profiler.cpp ROADMAP.md README.md
git commit -m "feat(profiling): per-frame SH-2 host time and SH-2 fork bookkeeping"
```

---

### Task 2: Headless frame benchmark `brimir_bench`

**Files:**
- Create: `tools/brimir_bench.cpp`
- Modify: `tools/CMakeLists.txt` (replace the whole file)
- Delete: `tools/benchmark_sh2.cpp`, `tools/run_benchmarks.ps1`
- Modify: `tools/README.md` (replace the whole file)
- Modify: `CMakeLists.txt` (IPO target lists: `benchmark_sh2` → `brimir_bench`)
- Modify: `build-all.ps1` (drop the benchmark switch and output check)

**Interfaces:**
- Consumes: `CoreWrapper::GetProfiler()`, profiler sample names `"Ymir_RunFrame"`, `"SH2_Master"`, `"SH2_Slave"` (Task 1); existing `CoreWrapper::Initialize`, `LoadIPLFromFile`, `LoadGame`, `LoadState`, `SaveState`, `GetStateSize`, `RunFrame`, `SetProfilingEnabled`, `ResetProfiling`, `GetLastError`.
- Produces: executable `brimir_bench` (output `build/bin/brimir_bench[.exe]`) with the CLI documented in `tools/README.md`. Exit code 0 on success, 1 on usage error, 2 on load failure.

- [ ] **Step 1: Write the tool**

Create `tools/brimir_bench.cpp`:

```cpp
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
```

- [ ] **Step 2: Replace the tools build file and remove the micro-benchmark**

Replace `tools/CMakeLists.txt` with:

```cmake
# Headless frame benchmark: runs real content without a frontend and reports
# host ms/frame and the SH-2 share of emulation time. See tools/README.md.
add_executable(brimir_bench brimir_bench.cpp)

target_link_libraries(brimir_bench PRIVATE brimir_bridge brimir::brimir-core)

target_compile_features(brimir_bench PRIVATE cxx_std_20)
```

Then:

```bash
git rm -q tools/benchmark_sh2.cpp tools/run_benchmarks.ps1
```

In the root `CMakeLists.txt`, replace `benchmark_sh2` with `brimir_bench` in both places (the `list(APPEND brimir_ipo_targets brimir-core benchmark_sh2)` line and the `foreach(target IN ITEMS ...)` line).

- [ ] **Step 3: Clean up `build-all.ps1`**

In `build-all.ps1`:
- remove the `.PARAMETER WithBenchmarks` help entry and its description line, and the `.EXAMPLE` block for `.\build-all.ps1 -WithBenchmarks`
- remove `[switch]$WithBenchmarks,` from `param(...)`
- remove the line `Write-Host "  Benchmarks: $(if ($WithBenchmarks) { 'Enabled' } else { 'Disabled' })" -ForegroundColor White`
- replace the `$Outputs = @{ ... }` block with:

```powershell
$Outputs = @{
    "Libretro Core" = "$BuildDir\bin\$BuildType\brimir_libretro.dll"
    "Frame Benchmark" = "$BuildDir\bin\$BuildType\brimir_bench.exe"
}
```

- remove the whole `# Step 5: Run benchmarks if requested` block (the `if ($WithBenchmarks -and ...) { ... }` statement)
- replace `Write-Host "  • Run benchmarks: .\tools\run_benchmarks.ps1" -ForegroundColor White` with `Write-Host "  • Benchmark: $BuildDir\bin\$BuildType\brimir_bench.exe --bios <file> --game <file>" -ForegroundColor White`

- [ ] **Step 4: Rewrite the tools README**

Replace `tools/README.md` with:

````markdown
# Brimir tools

## brimir_bench — headless frame benchmark

Runs real content without a frontend and reports host time per frame and the
share of emulation time spent in each SH-2 CPU. Used for the SH-2 JIT baseline
(`design/sh2-baseline.md`) and for before/after comparisons.

Build:

```powershell
cmake --build build --target brimir_bench
```

Benchmark a game from boot (no controller input, so it stays on the title or
attract screens):

```powershell
build\bin\brimir_bench.exe --bios system\sega_101.bin --game "D:\Saturn\Game.cue" --frames 1800
```

Benchmark gameplay: create a save state at the point of interest, then measure
from it. `--dump-at` runs N frames from the loaded content (and optional
`--state`) and writes the core's raw state data:

```powershell
build\bin\brimir_bench.exe --bios system\sega_101.bin --game Game.cue --dump-at 3600 --dump-state game.bstate
build\bin\brimir_bench.exe --bios system\sega_101.bin --game Game.cue --state game.bstate --frames 1800
```

RetroArch `.state` files are accepted only if they contain the raw core data
(no RetroArch container header, no compression); otherwise loading fails with an
error.

Output:

```
content      : Game.cue
frames       : 1800 (warmup 120)
ms/frame     : avg 7.912  p50 7.804  p95 9.120  p99 10.301  max 14.022
fps (host)   : 126.4
Ymir_RunFrame: 7.850 ms/frame
SH2 master   : 3.120 ms/frame (39.7% of Ymir_RunFrame)
SH2 slave    : 1.004 ms/frame (12.8% of Ymir_RunFrame)
SH2 total    : 4.124 ms/frame (52.5% of Ymir_RunFrame)
```

SH-2 time is measured inside `SH2::Advance` and includes on-chip peripherals
(DMA, timers) and bus accesses the CPUs make. VDP rendering runs on worker
threads by default, so `Ymir_RunFrame` is the emulation thread's time only.

Exit codes: 0 success, 1 usage error, 2 load failure.
````

- [ ] **Step 5: Build and smoke-test the tool**

Run: `cmake -S . -B build && cmake --build build --target brimir_bench brimir_tests`
Expected: builds with no errors.

Run (the BIOS fixture is local-only and untracked; use any Saturn BIOS you have):
`build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --frames 120 --warmup 30`
Expected: exit code 0 and output containing `content      : (BIOS menu)`, a non-zero `Ymir_RunFrame`, and non-zero `SH2 master`.

Run: `build\bin\brimir_bench.exe --frames 10`
Expected: `--bios is required`, the usage text, exit code 1.

Run: `build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --dump-at 60 --dump-state $env:TEMP\bench.bstate` then `build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --state $env:TEMP\bench.bstate --frames 60 --warmup 0`
Expected: first prints `Wrote save state after 60 frames`, second exits 0 with a report.

Run: `ctest --test-dir build --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 6: Commit**

```bash
git add tools/brimir_bench.cpp tools/CMakeLists.txt tools/README.md CMakeLists.txt build-all.ps1
git commit -m "feat(tools): add headless brimir_bench and drop the SH-2 micro-benchmark"
```

---

### Task 3: Baseline report

**Files:**
- Create: `design/sh2-baseline.md`

**Interfaces:**
- Consumes: `brimir_bench` (Task 2).
- Produces: the committed baseline report required by `design/sh2-jit.md` section 8.

This task needs content that is not in the repository (BIOS and game discs). The implementer must **stop and ask the user** for: the BIOS path, 4–6 game image paths covering 2D and 3D titles (at least one interlaced/high-resolution title such as *Virtua Fighter 2*, one 3D title using both SH-2s such as *Panzer Dragoon Zwei* or *Sega Rally*, and one 2D title), and optionally save states at gameplay points.

- [ ] **Step 1: Ask the user for content paths**

Ask for the BIOS path, game paths, and whether they have gameplay save states (or want `--dump-at` states created). Do not guess paths.

- [ ] **Step 2: Build an optimized benchmark binary**

Run: `cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DBRIMIR_LTO=ON -DBrimir_ENABLE_IPO=ON && cmake --build build-bench --target brimir_bench`
Expected: builds with no errors.

- [ ] **Step 3: Measure each title three times**

For each title run (with `--state` when a state exists):
`build-bench\bin\brimir_bench.exe --bios <bios> --game <game> --frames 1800 --warmup 300`
Run three times, keep the median `avg` ms/frame and the SH-2 shares from the median run. Record the machine (CPU model, core count, OS, compiler).

- [ ] **Step 4: Write the report**

Create `design/sh2-baseline.md` with this structure, filled with the measured numbers (no empty cells; write `n/a` only for data that genuinely does not apply):

```markdown
# SH-2 baseline (interpreter)

**Date**: <YYYY-MM-DD>
**Commit**: <git rev-parse --short HEAD>
**Machine**: <CPU, cores/threads, RAM, OS>
**Build**: Release, LTO + IPO, <compiler and version>
**Method**: `brimir_bench --frames 1800 --warmup 300`, median of 3 runs. SH-2 time is host time inside `SH2::Advance` (includes SH-2 DMA/timers and bus access). VDP rendering is threaded (defaults).

| Title | Scene | ms/frame avg | p95 | SH2 master ms (%) | SH2 slave ms (%) | SH2 total % |
|---|---|---|---|---|---|---|
| BIOS menu | boot menu | | | | | |
| <title> | <boot / state description> | | | | | |

## Observations

- <which titles are SH-2 bound vs VDP bound, based on the SH2 total %>
- <slave SH-2 usage pattern>

## Implications for the JIT

- <expected ceiling: if SH-2 total is X% of frame time, removing all SH-2 cost saves at most X%>
- <titles to use for milestone 1 validation (10-minute runs) and later speed comparisons>
```

- [ ] **Step 5: Commit**

```bash
git add design/sh2-baseline.md
git commit -m "docs(design): record SH-2 interpreter baseline"
```
