#pragma once

#include <brimir/core/types.hpp>

namespace brimir::state {

struct SCSPTimer {
    uint8 incrementInterval;
    uint8 reload;

    bool doReload;
    uint8 counter;
};

} // namespace brimir::state
