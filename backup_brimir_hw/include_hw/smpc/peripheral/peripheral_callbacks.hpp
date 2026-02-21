#pragma once

/**
@file
@brief Peripheral callbacks.
*/

#include "peripheral_report.hpp"

#include <brimir/util/callback.hpp>

namespace brimir::peripheral {

// Invoked when a peripheral requests a report.
using CBPeripheralReport = util::OptionalCallback<void(PeripheralReport &report)>;

} // namespace brimir::peripheral
