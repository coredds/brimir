// Throwaway harness for Brimir issue #7: drives the libretro API like RetroArch's
// Close Content and reports any C++ exception thrown (first chance) with its type,
// what() and a module+offset stack.
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>
#include "libretro.h"

#pragma comment(lib, "dbghelp.lib")

// All harness output goes to g_log so it works even when built as a GUI-subsystem app
// (no stdout), which is how RetroArch runs on Windows.
static FILE *g_log = stdout;
#define printf(...) (fprintf(g_log, __VA_ARGS__), fflush(g_log))

static std::string g_sysDir, g_saveDir;
static HMODULE g_core = nullptr;
static volatile LONG g_inUnload = 0;

static void log_cb(enum retro_log_level, const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); fflush(g_log);
}

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd & 0xFFFF) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((retro_log_callback *)data)->log = log_cb; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = g_sysDir.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = g_saveDir.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    default: return false;
    }
}
static void video_cb(const void *, unsigned, unsigned, size_t) {}
static void audio_cb(int16_t, int16_t) {}
static size_t audio_batch_cb(const int16_t *, size_t n) { return n; }
static void input_poll_cb() {}
static unsigned g_frame = 0;
static int16_t input_state_cb(unsigned port, unsigned, unsigned, unsigned id) {
    // Tap Start every ~4 s on port 0 to get past title screens.
    bool start = port == 0 && (g_frame % 240) < 10;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return start ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : 0;
    return (start && id == RETRO_DEVICE_ID_JOYPAD_START) ? 1 : 0;
}

static void print_stack(CONTEXT *ctx) {
    HANDLE proc = GetCurrentProcess();
    CONTEXT c = *ctx;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = c.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &c, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset)
            break;
        DWORD64 pc = sf.AddrPC.Offset;
        char modName[MAX_PATH] = "?";
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &mod);
        if (mod) GetModuleFileNameA(mod, modName, MAX_PATH);
        const char *base = strrchr(modName, '\\'); base = base ? base + 1 : modName;
        char symBuf[sizeof(SYMBOL_INFO) + 512]{};
        auto *sym = (SYMBOL_INFO *)symBuf; sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD ldisp = 0;
        bool hasSym = SymFromAddr(proc, pc, &disp, sym);
        bool hasLine = SymGetLineFromAddr64(proc, pc, &ldisp, &line);
        printf("    #%02d %s+0x%llx  %s%s", i, base, (unsigned long long)(pc - (DWORD64)mod),
               hasSym ? sym->Name : "", hasSym ? "" : "");
        if (hasLine) printf("  (%s:%lu)", line.FileName, line.LineNumber);
        printf("\n");
    }
    fflush(stdout);
}

// Decode MSVC C++ exception type name from the ThrowInfo (x64, image-relative RVAs).
static void describe_cxx(EXCEPTION_RECORD *er) {
    if (er->NumberParameters < 4) return;
    auto obj = (void *)er->ExceptionInformation[1];
    auto throwInfo = (const int32_t *)er->ExceptionInformation[2];
    auto imageBase = (const char *)er->ExceptionInformation[3];
    if (!throwInfo || !imageBase) return;
    // ThrowInfo: attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray
    auto cta = (const int32_t *)(imageBase + throwInfo[3]);
    int n = cta[0];
    for (int i = 0; i < n; ++i) {
        auto ct = (const int32_t *)(imageBase + cta[1 + i]);
        // CatchableType: properties, pType, ...
        auto td = imageBase + ct[1];
        const char *name = td + 16; // TypeDescriptor: pVFTable, spare, name[]
        printf("    catchable type: %s\n", name);
        if (strcmp(name, ".?AVexception@std@@") == 0 && obj) {
            printf("    what(): %s\n", ((std::exception *)obj)->what());
        }
    }
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0xE06D7363) {
        printf("\n*** C++ exception thrown (first chance) on thread %lu, inUnload=%ld\n", GetCurrentThreadId(),
               g_inUnload);
        describe_cxx(ep->ExceptionRecord);
        print_stack(ep->ContextRecord);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep) {
    printf("\n*** UNHANDLED exception 0x%08lx -> process would crash (this is the bug)\n",
           ep->ExceptionRecord->ExceptionCode);
    TerminateProcess(GetCurrentProcess(), 0xDEAD);
    return EXCEPTION_EXECUTE_HANDLER;
}

template <typename T> static T get(const char *n) {
    auto p = (T)GetProcAddress(g_core, n);
    if (!p) { printf("missing export %s\n", n); exit(2); }
    return p;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        printf("usage: harness <core.dll> <content> <system_dir> <save_dir> <frames> [cycles]\n");
        return 1;
    }
    if (const char *lp = getenv("HARNESS_LOG")) g_log = fopen(lp, "w");
    if (!g_log) return 3;
    g_sysDir = argv[3]; g_saveDir = argv[4];
    unsigned frames = (unsigned)atoi(argv[5]);
    int cycles = argc > 6 ? atoi(argv[6]) : 1;

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter(unhandled);

    // "gui" mode: emulate a GUI-subsystem frontend (RetroArch on Windows) where the core's
    // UCRT has no valid stdout/stderr. The harness itself uses the static CRT, whose stdout
    // handle is already initialized, so its own printf keeps working.
    if (argc > 7 && strcmp(argv[7], "gui") == 0) {
        printf("[harness] GUI mode: clearing std handles before loading the core\n");
        SetStdHandle(STD_OUTPUT_HANDLE, nullptr);
        SetStdHandle(STD_ERROR_HANDLE, nullptr);
    }
    g_core = LoadLibraryA(argv[1]);
    if (!g_core) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    SymRefreshModuleList(GetCurrentProcess());

    get<void (*)(retro_environment_t)>("retro_set_environment")(env_cb);
    get<void (*)(retro_video_refresh_t)>("retro_set_video_refresh")(video_cb);
    get<void (*)(retro_audio_sample_t)>("retro_set_audio_sample")(audio_cb);
    get<void (*)(retro_audio_sample_batch_t)>("retro_set_audio_sample_batch")(audio_batch_cb);
    get<void (*)(retro_input_poll_t)>("retro_set_input_poll")(input_poll_cb);
    get<void (*)(retro_input_state_t)>("retro_set_input_state")(input_state_cb);
    auto init = get<void (*)()>("retro_init");
    auto deinit = get<void (*)()>("retro_deinit");
    auto load = get<bool (*)(const retro_game_info *)>("retro_load_game");
    auto run = get<void (*)()>("retro_run");
    auto unload = get<void (*)()>("retro_unload_game");
    auto memData = get<void *(*)(unsigned)>("retro_get_memory_data");
    auto memSize = get<size_t (*)(unsigned)>("retro_get_memory_size");

    for (int c = 0; c < cycles; ++c) {
        printf("=== cycle %d ===\n", c);
        init();
        retro_game_info gi{argv[2], nullptr, 0, nullptr};
        if (!load(&gi)) { printf("load failed\n"); return 1; }
        DWORD t0 = GetTickCount();
        for (g_frame = 0; g_frame < frames; ++g_frame) run();
        printf("ran %u frames in %lu ms\n", frames, GetTickCount() - t0);
        // RetroArch Close Content: save SRAM via memory pointer, then unload, then deinit.
        printf("SRAM size %zu ptr %p\n", memSize(RETRO_MEMORY_SAVE_RAM), memData(RETRO_MEMORY_SAVE_RAM));
        if (getenv("HARNESS_PREFILL")) {
            // Shared UCRT stdout (harness is /MD): fill its buffer to a few bytes short of full,
            // so the first devlog line during unload must flush -> fails in a GUI process.
            fputc('.', stdout); // make sure the buffer is allocated
            char **base = nullptr, **ptr = nullptr; int *cnt = nullptr;
            _get_stream_buffer_pointers(stdout, &base, &ptr, &cnt);
            int room = (cnt && *base) ? *cnt : -1;
            if (room > 8) {
                std::string filler(room - 8, '.');
                fwrite(filler.data(), 1, filler.size(), stdout);
            }
            _get_stream_buffer_pointers(stdout, &base, &ptr, &cnt);
            printf("[harness] prefilled stdout buffer, room left=%d\n", cnt ? *cnt : -1);
        }
        InterlockedExchange(&g_inUnload, 1);
        unload();
        deinit();
        InterlockedExchange(&g_inUnload, 0);
        printf("unload+deinit OK\n");
    }
    FreeLibrary(g_core);
    printf("DONE OK\n");
    return 0;
}
