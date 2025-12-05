#pragma once

#include <brimir/core/configuration_defs.hpp>
#include <brimir/sys/clocks.hpp>
#include <brimir/sys/memory_defs.hpp>

#include <brimir/core/hash.hpp>

namespace brimir::state {

struct SystemState {
    core::config::sys::VideoStandard videoStandard;
    sys::ClockSpeed clockSpeed;
    bool slaveSH2Enabled;

    alignas(16) XXH128Hash iplRomHash;

    alignas(16) std::array<uint8, sys::kWRAMLowSize> WRAMLow;
    alignas(16) std::array<uint8, sys::kWRAMHighSize> WRAMHigh;
};

} // namespace brimir::state
