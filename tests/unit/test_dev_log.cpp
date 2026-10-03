// Brimir - devlog robustness tests
//
// Regression for GitHub issue #7: inside a GUI frontend (RetroArch on Windows) the core has
// no usable stdout. Writes to it fail once the stdio buffer fills, and devlog used to throw
// std::system_error from fmt::println. That exception escaped retro_unload_game and
// crashed RetroArch on Close Content. Logging must never throw.

#include "catch_amalgamated.hpp"

#include <ymir/util/dev_log.hpp>

#include <cstdio>
#include <string>

#ifdef _WIN32
    #include <io.h>
    #define BRIMIR_DUP _dup
    #define BRIMIR_DUP2 _dup2
    #define BRIMIR_CLOSE _close
    #define BRIMIR_FILENO _fileno
static constexpr const char *kNullDevice = "NUL";
#else
    #include <unistd.h>
    #define BRIMIR_DUP dup
    #define BRIMIR_DUP2 dup2
    #define BRIMIR_CLOSE close
    #define BRIMIR_FILENO fileno
static constexpr const char *kNullDevice = "/dev/null";
#endif

namespace {

struct test_devlog_grp {
    static constexpr bool enabled = true;
    static constexpr devlog::Level level = devlog::level::trace;
    static constexpr std::string_view name = "Test";
};

// Makes stdout unwritable for the lifetime of the object (every write fails, like stdout
// in a GUI-subsystem frontend), then restores the original stdout.
class UnwritableStdout {
public:
    UnwritableStdout() {
        std::fflush(stdout);
        m_savedFd = BRIMIR_DUP(BRIMIR_FILENO(stdout));
        // Reopen stdout read-only: any fwrite to it now fails.
        m_ok = m_savedFd >= 0 && std::freopen(kNullDevice, "r", stdout) != nullptr;
    }

    ~UnwritableStdout() {
        if (m_savedFd < 0) {
            return;
        }
        std::clearerr(stdout);
        // Make stdout writable again, then point its descriptor back at the original target.
        if (std::freopen(kNullDevice, "w", stdout) != nullptr) {
            BRIMIR_DUP2(m_savedFd, BRIMIR_FILENO(stdout));
        }
        BRIMIR_CLOSE(m_savedFd);
    }

    bool Ok() const {
        return m_ok;
    }

private:
    int m_savedFd = -1;
    bool m_ok = false;
};

} // namespace

TEST_CASE("devlog does not throw when stdout cannot be written", "[devlog][regression]") {
    if (!devlog::debug_enabled<test_devlog_grp>) {
        SKIP("devlog is compiled out (Brimir_ENABLE_DEVLOG=OFF)");
    }

    // Long enough to overflow any stdio buffer, so a buffered write must hit the OS.
    const std::string payload(8192, 'x');

    // Catch2 reports through stdout, so nothing is asserted until stdout is restored.
    bool redirected = false;
    std::string thrown;
    {
        UnwritableStdout guard;
        redirected = guard.Ok();
        try {
            devlog::info<test_devlog_grp>("unload message");
            devlog::debug<test_devlog_grp>("large message {}", payload);
            for (int i = 0; i < 100; ++i) {
                devlog::debug<test_devlog_grp>("repeated message {} {}", i, payload);
            }
        } catch (const std::exception &e) {
            thrown = e.what();
        } catch (...) {
            thrown = "unknown exception";
        }
    }

    REQUIRE(redirected);
    INFO("devlog threw: " << thrown);
    CHECK(thrown.empty());
}
