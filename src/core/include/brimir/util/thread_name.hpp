#pragma once

/**
@file
@brief Defines the `util::SetCurrentThreadName` function to rename the current thread.
*/

namespace util {

/// @brief Changes the name of the current thread.
/// @param[in] threadName the new thread name
void SetCurrentThreadName(const char *threadName);

} // namespace util
