#pragma once

/**
@file
@brief Process-wide utility functions, including priority control and executable path query.
*/

#include <filesystem>

namespace util {

/// @brief Retrieves the path to the executable of this process.
/// @return the path to the executable
std::filesystem::path GetCurrentProcessExecutablePath();

/// @brief Enables or disables the priority boost for the current process.
/// @param[in] boost `true` to boost the priority of the current process, `false` to return to normal
void BoostCurrentProcessPriority(bool boost);

/// @brief Enables or disables the priority boost for the current thread.
/// @param[in] boost `true` to boost the priority of the current thread, `false` to return to normal
void BoostCurrentThreadPriority(bool boost);

} // namespace util
