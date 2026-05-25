#pragma once

/**
@file
@brief Compile-time `for` loop construct.
*/

#include <utility>

namespace util {

namespace detail {

    /// @brief `constexpr for` loop implementation.
    /// @tparam F the type of the function to invoke on every iteration
    /// @tparam ...S the index sequence to iterate over
    /// @param[in] function the function to invoke on every iteration
    template <typename F, std::size_t... S>
    inline constexpr void constexpr_for_impl(F &&function, std::index_sequence<S...>) {
        (function(std::integral_constant<std::size_t, S>{}), ...);
    }

} // namespace detail

/// @brief Invokes the function at compile-time for the specified number of iterations.
///
/// The function receives the iteration index as its sole argument.
///
/// Usage:
/// ```cpp
/// constexpr std::array<int, 10> items;
/// util::constexpr_for<10>([&](auto index) {
///     arr[index] = CompileTimeComputation(index);
/// });
/// ```
///
/// @tparam F the type of the function to invoke on every iteration
/// @tparam iterations the number of iterations to execute
/// @param[in] function the function to invoke on every iteration
template <std::size_t iterations, typename F>
inline constexpr void constexpr_for(F &&function) {
    detail::constexpr_for_impl(std::forward<F>(function), std::make_index_sequence<iterations>());
}

} // namespace util
