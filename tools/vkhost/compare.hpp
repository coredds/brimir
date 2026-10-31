// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// --compare: runs the same content once on the software renderer and once on the Vulkan renderer, each in its own
// child process (a copy of this executable), and compares the per-frame hashes. See tools/README.md.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace vkhost {

struct CompareConfig {
    std::vector<std::string> args; // the command line after argv[0], including --compare
    std::string systemDir;         // --system-dir (copied per child run; may be empty)
    std::string dumpDir;           // --dump-dir: where the differing frames go (default: a kept temp directory)
    int frames = 300;
    std::vector<std::pair<std::string, std::string>> vulkanOptions; // --compare-vulkan-option, Vulkan child only
    int stateEvery = 0; // --compare-state N: both children save a state after every Nth recorded frame (0: off)
    bool strictStates = false; // --fixed-date: brimir_state_diff --strict (every section of each pair identical)
};

// Whether the argument after --fixed-date is its (optional) value: "1" or a date starting YYYY-; the core validates
// the date itself.
inline bool IsFixedDateValue(const std::string& text) {
    if (text == "1") {
        return true;
    }
    return text.size() >= 5 && text[4] == '-' && text.find_first_not_of("0123456789") == 4;
}

// Returns 0 when every frame (and with stateEvery every state pair) is identical, 3 on any difference or failed check
// (the first differing frame is reported with its first differing pixel and both images are dumped as PPM; the first
// differing state pair's brimir_state_diff output is printed and both states are copied into the dump directory), 4
// when Vulkan is unavailable, 2 on setup failure.
int RunCompare(const CompareConfig& config);

} // namespace vkhost
