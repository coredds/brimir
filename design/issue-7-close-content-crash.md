# Issue #7: RetroArch crash on Close Content (Windows)

**Issue**: https://github.com/coredds/brimir/issues/7 (reported 2026-09-26 against v0.5.4)
**Status**: fixed in v0.5.5 (2026-10-03). **Open, waiting for the reporter to confirm.** Follow-up work (section 7) is on hold until then.
**Fix commits**: `96a5f42` (master), `63b25f7` (release/v0.5.5, cherry-pick onto v0.5.4); version bump `6e6ce15`, tag `v0.5.5`; merged back to master in `dbd3a62`.
**Reply to reporter**: https://github.com/coredds/brimir/issues/7#issuecomment-5972461489

## 1. Report

Panzer Dragoon II Zwei (USA, CHD), US BIOS `mpr-17933.bin`, RetroArch 1.22.2 (Steam), Windows 11 x64, Vulkan. After about a minute of play, Quick Menu > Close Content closes RetroArch instead of returning to the menu. The `.srm` is saved first; the last log line is `[libretro INFO] [Brimir] Unloading game`. Windows Application Error: `KERNELBASE.dll`, exception `0xe06d7363` (unhandled MSVC C++ exception), fault offset `0xc41ca`.

## 2. Root cause

1. Ymir's dev log (`src/core/include/ymir/util/dev_log.hpp`) prints with `fmt::println` to stdout. It is compiled into release builds (`Brimir_ENABLE_DEVLOG ON` in `CMakeLists.txt`).
2. `fmt::println` throws `std::system_error` ("cannot write to file") when `fwrite` writes less than requested (`fwrite_all` in `vendor/fmt/include/fmt/format-inl.h`).
3. `retroarch.exe` is a Windows GUI-subsystem program (MinGW, `msvcrt.dll`); the core uses the separate UCRT. In such a process the UCRT stdout has no handle (`_fileno(stdout) == -2`). Writes are buffered and succeed until the 4096-byte buffer is full; the flush then fails with `errno = 9` (EBADF) and `fwrite` returns a short count (measured with `issue-7/probe.cpp`: short write after exactly 4080 + 16 bytes).
4. So the dev log call that crosses the 4 KB mark throws. Where it lands depends on how much was logged during the session:
   - inside `retro_run`: caught by the catch-all in `CoreWrapper::RunFrame`, which silently aborts the rest of that frame;
   - inside `retro_unload_game`: nothing catches it, it crosses the libretro C ABI into RetroArch and the process dies. The unload path logs "Disabling threaded VDP1/VDP2 rendering", "Tray opened" and "Ejected disc", which is what pushed the reporter's session over the mark.

This explains why the crash is intermittent and why nothing is logged after `Unloading game`.

## 3. Reproduction

Machine: AMD Ryzen 7 5700G, Windows 11 Pro 10.0.26300, MSVC 19.44 (VS 2022). Content: Panzer Dragoon II Zwei (USA) CHD, US BIOS only (to match the report).

Tools are in `design/issue-7/` (throwaway diagnostics, not part of the build):

- **`harness.cpp`**: loads a core DLL and drives the libretro API like RetroArch's Close Content (`retro_init`, `retro_load_game`, N × `retro_run`, `retro_unload_game`, `retro_deinit`, optionally several cycles). A vectored exception handler prints every first-chance C++ exception (decoded type, `what()`, stack); an unhandled-exception filter reports the crash. Build it as a GUI program with the dynamic CRT so it shares the core's UCRT stdout:
  `cl /EHsc /MD /O1 /Zi /std:c++17 /I include harness.cpp /Fe:harness_gui.exe /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup`
  Run with `HARNESS_LOG=<file>` (its own output goes there). Arguments: `<core.dll> <content> <system_dir> <save_dir> <frames> [cycles]`. `HARNESS_PREFILL=1` fills the shared stdout buffer to 8 bytes short of full right before `retro_unload_game`, so the first dev log line during unload must flush. This reproduces the reported crash deterministically.
- **`probe.cpp`**: shows the UCRT stdout behaviour in a GUI process (same build flags).
- **`ra_test.ps1`**: runs the real RetroArch with a core passed via `-L`, sandboxed through `--appendconfig` (temp save/state/system dirs, history and config saving off, the user's RTC file copied in so the BIOS doesn't stop at the clock screen), then sends `CLOSE_CONTENT` / `QUIT` over the network command interface. Paths are specific to the dev machine.

Results:

| Build | Scenario | Result |
|---|---|---|
| v0.5.4 release DLL | console harness, 3600 frames | no exception (stdout valid) |
| v0.5.4 release DLL | GUI harness, 3 cycles × 1200 frames | `std::system_error: cannot write to file: bad file descriptor` thrown in `retro_run` (cycle 2), swallowed by `RunFrame` |
| v0.5.4 release DLL | GUI harness, `HARNESS_PREFILL=1` | thrown in `retro_unload_game`, **unhandled `0xe06d7363` from `KERNELBASE.dll+0xc41ca`**: identical to the report |
| fixed build / v0.5.5 release DLL | GUI harness, `HARNESS_PREFILL=1`, 2 cycles | exception caught inside the dev log, clean unload and shutdown |
| v0.5.4 release DLL | RetroArch 1.22.2, 60 s (also 40 s + 2 resets) + Close Content | no crash: the 4 KB mark did not fall inside unload in these runs |
| fixed build / v0.5.5 release DLL | RetroArch 1.22.2, 60 s + Close Content | clean unload, core symbols unloaded, RetroArch stays up |

Note: this RetroArch build crashes (access violation inside `retroarch.exe`) when it receives the `GET_STATUS` network command, so `ra_test.ps1` doesn't use it. That is a RetroArch issue, unrelated to the core.

## 4. Fix

- `dev_log.hpp`: both `devlog::detail::log` overloads wrap formatting and printing in `try { ... } catch (...) {}`. Dev logs are best-effort; they must never unwind emulator code. fmt still throws internally when stdout is broken (about once per 4 KB of output), but the exception stays inside the logger.
- `libretro.cpp`: `retro_unload_game` and `retro_deinit` catch `std::exception` / `...` and report them through the libretro log (`Exception while unloading game: ...`, `Exception while shutting down: ...`), so no exception can cross the C ABI from these entry points. On the hotfix branch the v0.5.4 lock structure is kept; only the try/catch was added.
- `tests/unit/test_dev_log.cpp`: reopens stdout read-only, logs (including 8 KB messages, 100 times), restores stdout and only then asserts that nothing was thrown. It failed before the fix with `cannot write to file`.

## 5. Verification

- v0.5.5 (hotfix branch): 84 test cases, 647,798 assertions pass (Windows x64, MSVC 2022, Release).
- master: full CTest suite (`brimir_tests`, `brimir_tests_jit_ir`) passes; CI green on `dbd3a62`.
- Release workflow for `v0.5.5` succeeded; all four platform packages published. The published Windows DLL was re-tested with the GUI harness (prefill) and in RetroArch (section 3).

## 6. Release

v0.5.5 is a hotfix built from the v0.5.4 tag on branch `release/v0.5.5`, containing only this fix plus version metadata. Master's unreleased work (SH-2 JIT, backup RAM rework, thread-safety fixes) is not in it.

## 7. Follow-ups (on hold until the reporter replies)

If the reporter confirms, close #7. If it still fails for them, the RetroArch log should now contain `[Brimir] Exception while unloading game: ...`, which would point to a different cause.

Candidate follow-ups, in priority order:

1. **Remaining throwing prints**: `src/core/src/ymir/media/loader/loader_mdf_mds.cpp:202` and `src/core/src/ymir/hw/m68k/m68k.cpp:1092` call `fmt::println` directly and can throw the same way (rare error paths).
2. **Silent mid-frame aborts**: `CoreWrapper::RunFrame` catches everything, stores `m_lastError` and returns; nothing reports it. Log it through the libretro log.
3. **C ABI audit**: check the other libretro entry points (`retro_run`, `retro_load_game`, `retro_serialize` / `retro_unserialize`, disk control callbacks) and the VDP render threads, where an escaping exception calls `std::terminate`.
4. **Dev log in release builds**: either route warn+ levels to the libretro log (diagnostics in user bug reports) or set `Brimir_ENABLE_DEVLOG OFF` for release builds (output is invisible in RetroArch anyway; avoids formatting cost). Measure with `brimir_bench`.
5. **Regression test at the libretro level**: init/load/run/unload with stdout unwritable, like the harness. Needs a BIOS, which CI doesn't have.
6. **Release process**: release notes are hardcoded in `.github/workflows/release.yml` (edited every release, conflicts on merges); generate them from the CHANGELOG section instead. Update actions still on Node.js 20; `ubuntu-latest` moves to Ubuntu 26 on 2026-10-19.
7. **Optional**: report the `GET_STATUS` crash to RetroArch.
