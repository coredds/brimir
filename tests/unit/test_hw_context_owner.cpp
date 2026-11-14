// The libretro hardware context has one owner per process (src/libretro/hw_context_owner.hpp): RetroArch's run-ahead
// second instance is another copy of the library in the same process and must see the main instance's claim.

#include "catch_amalgamated.hpp"

#include "hw_context_owner.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

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

using namespace brimir::libretro;

namespace {

// Two loaded copies of the library: one global each.
const int g_copyA = 0;
const int g_copyB = 0;

void SetVariable(const char* value) {
#ifdef _WIN32
    SetEnvironmentVariableA(kHwContextRegistryVariable, value);
#else
    if (value != nullptr) {
        setenv(kHwContextRegistryVariable, value, 1);
    } else {
        unsetenv(kHwContextRegistryVariable);
    }
#endif
}

// A fresh process as far as the registry is concerned (the old block stays allocated: it is never freed)
void ClearVariable() {
    SetVariable(nullptr);
}

std::string ReadVariable() {
#ifdef _WIN32
    char value[128];
    const DWORD length = GetEnvironmentVariableA(kHwContextRegistryVariable, value, sizeof(value));
    return length > 0 && length < sizeof(value) ? std::string(value, length) : std::string();
#else
    const char* value = std::getenv(kHwContextRegistryVariable);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

unsigned long long ThisProcess() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long long>(getpid());
#endif
}

} // namespace

TEST_CASE("hardware context: no owner, nobody else holds it", "[hw-context-owner]") {
    ClearVariable();
    CHECK_FALSE(HwContextHeldByOther(&g_copyA));
    CHECK_FALSE(HwContextHeldByOther(&g_copyB));
    ReleaseHwContext(&g_copyA);
    CHECK(ReadVariable().empty()); // only a claim creates the registry
}

TEST_CASE("hardware context: a claim is seen by the other copy only", "[hw-context-owner]") {
    ClearVariable();
    ClaimHwContext(&g_copyA);
    CHECK_FALSE(HwContextHeldByOther(&g_copyA));
    CHECK(HwContextHeldByOther(&g_copyB));
    ReleaseHwContext(&g_copyA);
    CHECK_FALSE(HwContextHeldByOther(&g_copyB));
    CHECK_FALSE(HwContextHeldByOther(&g_copyA));
}

TEST_CASE("hardware context: only the owner releases its claim", "[hw-context-owner]") {
    ClearVariable();
    ClaimHwContext(&g_copyA);
    ReleaseHwContext(&g_copyB); // the declined second instance unloading (RetroArch calls retro_unload_game anyway)
    CHECK(HwContextHeldByOther(&g_copyB));
    ReleaseHwContext(&g_copyA);
    CHECK_FALSE(HwContextHeldByOther(&g_copyB));
}

TEST_CASE("hardware context: the environment is written once per process", "[hw-context-owner]") {
    // Later claims and releases (content reloads, a new copy of the core) change the registry, not the environment
    ClearVariable();
    ClaimHwContext(&g_copyA);
    const std::string published = ReadVariable();
    REQUIRE(published.rfind(std::to_string(ThisProcess()) + ":", 0) == 0);
    ReleaseHwContext(&g_copyA);
    ClaimHwContext(&g_copyB);
    CHECK(ReadVariable() == published);
    CHECK(HwContextHeldByOther(&g_copyA));
    ReleaseHwContext(&g_copyB);
    ClaimHwContext(&g_copyA);
    CHECK(ReadVariable() == published);
    ReleaseHwContext(&g_copyA);
    CHECK_FALSE(HwContextHeldByOther(&g_copyB));
}

TEST_CASE("hardware context: a registry inherited from another process is ignored", "[hw-context-owner]") {
    // A child process (brimir_vkhost --compare's children, anything RetroArch launches) inherits the environment.
    ClearVariable();
    ClaimHwContext(&g_copyA);
    const std::string published = ReadVariable();
    const std::string address = published.substr(published.find(':'));
    SetVariable((std::to_string(ThisProcess() + 1) + address).c_str()); // the same block, another process id
    CHECK_FALSE(HwContextHeldByOther(&g_copyB));
    ClaimHwContext(&g_copyB); // a new registry for this process
    CHECK(ReadVariable().rfind(std::to_string(ThisProcess()) + ":", 0) == 0);
    CHECK(HwContextHeldByOther(&g_copyA));
    ReleaseHwContext(&g_copyB);
    ClearVariable();
}

TEST_CASE("hardware context: a malformed or foreign value is no registry", "[hw-context-owner]") {
    const uint64_t notARegistry[4] = {};
    char foreign[96];
    std::snprintf(foreign, sizeof(foreign), "%llu:%llx:0", ThisProcess(),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&notARegistry)));
    const std::string pid = std::to_string(ThisProcess());
    for (const std::string& value :
         {std::string("garbage"), pid, pid + ":", pid + ":zz", pid + ":0", pid + ":0:0", pid + ":12x",
          pid + ":1000", pid + ":1000:", pid + ":1000:zz", pid + ":1000:1x", pid + ":1001:1", std::string(":1234"),
          std::string(foreign)}) {
        CAPTURE(value);
        SetVariable(value.c_str());
        CHECK_FALSE(HwContextHeldByOther(&g_copyB));
        ReleaseHwContext(&g_copyA);
    }
    ClearVariable();
}

namespace {

// A page that was mapped and is not any more: where a previous process image's registry was
struct UnmappedPage {
    std::uintptr_t address = 0;
    UnmappedPage() {
#ifdef _WIN32
        void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        REQUIRE(page != nullptr);
        address = reinterpret_cast<std::uintptr_t>(page);
        REQUIRE(VirtualFree(page, 0, MEM_RELEASE));
#else
        void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(page != MAP_FAILED);
        address = reinterpret_cast<std::uintptr_t>(page);
        REQUIRE(munmap(page, 4096) == 0);
#endif
    }
};

} // namespace

TEST_CASE("hardware context: a stale value from a previous process image is no registry", "[hw-context-owner]") {
    // exec keeps the process id and the environment (RetroArch's "Restart RetroArch" on Linux): the variable names an
    // address of the previous image. It must neither crash nor make this image's first copy decline.
    const std::string pid = std::to_string(ThisProcess());

    SECTION("the address is unmapped now") {
        const UnmappedPage page;
        char value[96];
        std::snprintf(value, sizeof(value), "%s:%llx:1234abcd", pid.c_str(),
                      static_cast<unsigned long long>(page.address));
        SetVariable(value);
    }
    SECTION("the address is mapped, by something without the magic") {
        static const uint64_t unrelated[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        char value[96];
        std::snprintf(value, sizeof(value), "%s:%llx:2", pid.c_str(),
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&unrelated[0])));
        SetVariable(value);
    }
    SECTION("the address holds a registry with another nonce, held by another copy") {
        // the worst case: a block with the magic and an owner, not the one the variable was published with
        ClearVariable();
        ClaimHwContext(&g_copyA);
        const std::string published = ReadVariable();
        const size_t nonceAt = published.rfind(':') + 1;
        const unsigned long long nonce = std::strtoull(published.c_str() + nonceAt, nullptr, 16);
        char value[96];
        std::snprintf(value, sizeof(value), "%s%llx", published.substr(0, nonceAt).c_str(), nonce + 1);
        SetVariable(value);
    }

    const std::string stale = ReadVariable();
    CHECK_FALSE(HwContextHeldByOther(&g_copyA));
    CHECK_FALSE(HwContextHeldByOther(&g_copyB)); // the fresh main instance loads
    ReleaseHwContext(&g_copyA);                  // unload and deinit with the stale value
    CHECK(ReadVariable() == stale);
    ClaimHwContext(&g_copyB); // the main instance's SET_HW_RENDER accepted: a registry for this image
    CHECK(ReadVariable() != stale);
    CHECK(ReadVariable().rfind(pid + ":", 0) == 0);
    CHECK(HwContextHeldByOther(&g_copyA)); // its second instance declines
    CHECK_FALSE(HwContextHeldByOther(&g_copyB));
    ReleaseHwContext(&g_copyB);
    CHECK_FALSE(HwContextHeldByOther(&g_copyA));
    ClearVariable();
}