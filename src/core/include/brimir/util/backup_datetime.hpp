#pragma once

/**
@file
@brief Utilities for dealing with Saturn backup memory date/time values.
*/

#include <brimir/core/types.hpp>

namespace util {

/// @brief A structured representation of the Saturn's backup memory date/time value.
struct BackupDateTime {
    /// @brief Constructs a backup memory date/time value from a raw value.
    /// @param[in] raw the raw value
    BackupDateTime(uint32 raw) noexcept;

    uint32 year;   ///< The full year (e.g. 1994, 2025, etc.)
    uint32 month;  ///< The month of the year, from 1 to 12
    uint32 day;    ///< The day of the month, from 1 to 31
    uint32 hour;   ///< The hour of the day, from 00 (midnight) to 23
    uint32 minute; ///< The minutes in the hour, from 00 to 59

    /// @brief Converts the date/time values from this struct into the raw representation used by the Saturn.
    /// @return the raw representation of this backup memory date/time
    [[nodiscard]] uint32 ToRaw() const noexcept;
};

} // namespace util
