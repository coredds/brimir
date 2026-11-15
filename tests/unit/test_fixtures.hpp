#pragma once

#include <cstdlib>
#include <filesystem>

namespace brimir::test {

// The tests/fixtures folder: BRIMIR_TEST_FIXTURES_DIR when set (a test binary run away from its source tree, e.g. on
// an Android device), otherwise the folder in the source tree this binary was built from.
inline std::filesystem::path FixturesDir() {
    if (const char *dir = std::getenv("BRIMIR_TEST_FIXTURES_DIR"); dir != nullptr && *dir != '\0') {
        return std::filesystem::path(dir);
    }
    // tests/unit/<this file> -> tests/fixtures
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures";
}

} // namespace brimir::test
