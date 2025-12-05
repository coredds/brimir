#pragma once

/**
@file
@brief Defines the `util::unreachable()` function that marks code as unreachable.
*/

namespace util {

/// @brief Marks a point in code as unreachable.
///
/// This is a compiler hint that should be used in places where it is never expected to be reached.
/// It invokes undefined behavior, which may lead to crashes if execution *does* reach this function.
[[noreturn]] inline void unreachable() {
#if defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
    __builtin_unreachable();
#elif defined(_MSC_VER)
    __assume(0);
#endif
}

} // namespace util
