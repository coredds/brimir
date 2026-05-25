#pragma once

/**
@file
@brief Peripheral callbacks.
*/

#include "peripheral_report.hpp"

#include <ymir/util/callback.hpp>

namespace ymir::peripheral {

// Invoked when a peripheral requests a report.
using CBPeripheralReport = util::OptionalCallback<void(PeripheralReport &report)>;

} // namespace ymir::peripheral
