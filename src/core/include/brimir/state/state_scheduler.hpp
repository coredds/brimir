#pragma once

#include <brimir/core/scheduler_defs.hpp>

#include <brimir/core/types.hpp>

namespace brimir::state {

struct SchedulerState {
    struct EventState {
        uint64 target;
        uint64 countNumerator;
        uint64 countDenominator;
        core::UserEventID id;
    };

    uint64 currCount;
    alignas(16) std::array<EventState, core::kNumScheduledEvents> events;
};

} // namespace brimir::state
