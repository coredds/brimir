#include "hw_context_owner.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
#endif

namespace brimir::libretro {

namespace {

struct Registry {
    uint64_t magic;
    uint64_t nonce; // random, also in the variable: tells this process image's block from anything else
    std::atomic<std::uintptr_t> owner; // the holding copy's token, 0 for none
};
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free,
              "the registry is shared by separately compiled copies of the library");

constexpr uint64_t kRegistryMagic = 0x3143574852494D42ULL; // "BRIMHWC1"

unsigned long long ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long long>(getpid());
#endif
}

// The Win32 environment block, not the C runtime's copy: each copy of the library may have its own runtime.
bool ReadVariable(char* value, size_t capacity) {
#ifdef _WIN32
    const DWORD length = GetEnvironmentVariableA(kHwContextRegistryVariable, value, static_cast<DWORD>(capacity));
    return length > 0 && length < capacity;
#else
    const char* text = std::getenv(kHwContextRegistryVariable);
    if (text == nullptr) {
        return false;
    }
    const int length = std::snprintf(value, capacity, "%s", text);
    return length > 0 && static_cast<size_t>(length) < capacity;
#endif
}

// Copies size bytes at address into out, or returns false if they are not readable, without touching them from this
// thread: the kernel does the copy and reports a bad address instead of faulting.
bool ReadMemorySafely(std::uintptr_t address, void* out, size_t size) {
#ifdef _WIN32
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &read) &&
           read == size;
#else
    // write() from an unmapped or unreadable address fails with EFAULT (Linux, macOS, the BSDs); the pipe holds far
    // more than the registry
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    ssize_t written;
    do {
        written = write(fds[1], reinterpret_cast<const void*>(address), size);
    } while (written < 0 && errno == EINTR);
    bool ok = written == static_cast<ssize_t>(size);
    if (ok) {
        ssize_t got;
        do {
            got = read(fds[0], out, size);
        } while (got < 0 && errno == EINTR);
        ok = got == static_cast<ssize_t>(size);
    }
    close(fds[0]);
    close(fds[1]);
    return ok;
#endif
}

// The variable is "<process id>:<address>:<nonce>". It is inherited by a child process (another id) and survives
// exec, which keeps the id and the environment while the address now means nothing (RetroArch's "Restart RetroArch"
// on Linux execs itself): the address is read only through ReadMemorySafely, and only a block holding the magic and
// the variable's random nonce is taken. A stale value is no registry: nobody holds the context, and the next claim
// publishes a new block.
Registry* FindRegistry() {
    char value[96];
    if (!ReadVariable(value, sizeof(value))) {
        return nullptr;
    }
    char* end = nullptr;
    const unsigned long long pid = std::strtoull(value, &end, 10);
    if (end == value || *end != ':' || pid != ProcessId()) {
        return nullptr; // inherited from the parent process (or malformed)
    }
    const char* field = end + 1;
    const unsigned long long pointer = std::strtoull(field, &end, 16);
    if (end == field || *end != ':' || pointer == 0) {
        return nullptr;
    }
    field = end + 1;
    const unsigned long long nonce = std::strtoull(field, &end, 16);
    if (end == field || *end != '\0') {
        return nullptr;
    }
    const auto address = static_cast<std::uintptr_t>(pointer);
    if (address % alignof(Registry) != 0) {
        return nullptr;
    }
    uint64_t header[2];
    if (!ReadMemorySafely(address, header, sizeof(header)) || header[0] != kRegistryMagic || header[1] != nonce) {
        return nullptr;
    }
    return reinterpret_cast<Registry*>(address);
}

uint64_t NewNonce() {
    uint64_t nonce = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        std::random_device device;
        nonce ^= (static_cast<uint64_t>(device()) << 32) ^ device();
    } catch (...) {
        // the clock alone
    }
    return nonce;
}

// Memory of the process, not of this copy: it stays valid after the copy is unloaded (a statically linked C runtime
// would destroy its own heap with it).
Registry* CreateRegistry() {
#ifdef _WIN32
    void* memory = VirtualAlloc(nullptr, sizeof(Registry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (memory == nullptr) {
        return nullptr;
    }
#else
    void* memory = mmap(nullptr, sizeof(Registry), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        return nullptr;
    }
#endif
    auto* registry = new (memory) Registry{kRegistryMagic, NewNonce(), {0}};
    char value[96];
    std::snprintf(value, sizeof(value), "%llu:%llx:%llx", ProcessId(),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(registry)),
                  static_cast<unsigned long long>(registry->nonce));
#ifdef _WIN32
    SetEnvironmentVariableA(kHwContextRegistryVariable, value);
#else
    setenv(kHwContextRegistryVariable, value, 1);
#endif
    return registry;
}

std::uintptr_t Token(const void* self) {
    return reinterpret_cast<std::uintptr_t>(self);
}

} // namespace

bool HwContextHeldByOther(const void* self) {
    const Registry* registry = FindRegistry();
    if (registry == nullptr) {
        return false;
    }
    const std::uintptr_t owner = registry->owner.load();
    return owner != 0 && owner != Token(self);
}

void ClaimHwContext(const void* self) {
    Registry* registry = FindRegistry();
    if (registry == nullptr) {
        registry = CreateRegistry();
    }
    if (registry != nullptr) {
        registry->owner.store(Token(self));
    }
}

void ReleaseHwContext(const void* self) {
    if (Registry* registry = FindRegistry()) {
        std::uintptr_t expected = Token(self);
        registry->owner.compare_exchange_strong(expected, 0);
    }
}

} // namespace brimir::libretro
