#pragma once

/**
@file
@brief Useful arithmetic operations.
*/

#include "inline.hpp"

#include <concepts>

namespace util {

/// @brief Computes `base` to the power of `exp` at compile time where both are integral types.
/// @tparam T the type of `base`
/// @tparam U the type of `exp`
/// @param[in] base the base
/// @param[in] exp the exponent
/// @return `base` to the power of `exp`
template <std::integral T, std::unsigned_integral U>
[[nodiscard]] FORCE_INLINE constexpr auto ipow(T base, U exp) noexcept {
    if (exp == 0) {
        return 1;
    }
    const auto val = ipow(base, exp / 2);
    auto sqrVal = val * val;
    if (exp & 1) {
        sqrVal *= base;
    }
    return sqrVal;
}

/// @brief Converts `value` into the corresponding binary-coded decimal representation (BCD).
///
/// The number of digits is limited to the number of nibbles available in the return type `R`.
///
/// @tparam T the type of `value`
/// @tparam R the type of the result
/// @param[in] value the value to convert
/// @return `value` converted to BCD
template <std::integral T, std::integral R = T>
[[nodiscard]] FORCE_INLINE constexpr R to_bcd(T value) noexcept {
    // TODO: check behavior with signed integers
    constexpr auto numDigits = sizeof(R) * 2u;
    constexpr T mod = ipow(10, numDigits);

    value %= mod;
    R output = 0;
    R divisor = 1;
    for (int i = 0; i < numDigits; i++) {
        output |= (value / divisor % 10) << (4 * i);
        divisor *= 10;
    }
    return output;
}

/// @brief Converts `value` from binary-coded decimal representation (BCD) to hexadecimal.
/// @tparam T the type of `value`
/// @tparam R the type of the result
/// @param[in] value the value to convert
/// @return `value` converted from BCD
template <std::integral T, std::integral R = T>
[[nodiscard]] FORCE_INLINE constexpr R from_bcd(T value) noexcept {
    constexpr auto numDigits = sizeof(R) * 2u;

    R output = 0;
    R factor = 1;
    for (int i = 0; i < numDigits; i++) {
        output += ((value >> (4 * i)) & 0xF) * factor;
        factor *= 10;
    }
    return output;
}

} // namespace util
