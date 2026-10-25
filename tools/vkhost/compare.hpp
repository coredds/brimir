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
};

// Returns 0 when every frame is identical, 3 on any difference or failed check (the first differing frame is
// reported with its first differing pixel and both images are dumped as PPM), 4 when Vulkan is unavailable, 2 on
// setup failure.
int RunCompare(const CompareConfig& config);

} // namespace vkhost
