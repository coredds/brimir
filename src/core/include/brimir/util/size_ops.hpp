#pragma once

/**
@file
@brief User-defined literals and converters for binary sizes.
*/

/// @brief Expands the value to kibibytes.
/// @param[in] sz the value to convert
/// @return sz * 1024
inline constexpr unsigned long long operator""_KiB(unsigned long long sz) {
    return sz * 1024;
}

/// @brief Expands the value to mebibytes.
/// @param[in] sz the value to convert
/// @return sz * 1024 * 1024
inline constexpr unsigned long long operator""_MiB(unsigned long long sz) {
    return sz * 1024 * 1024;
}

/// @brief Expands the value to gibibytes.
/// @param[in] sz the value to convert
/// @return sz * 1024 * 1024 * 1024
inline constexpr unsigned long long operator""_GiB(unsigned long long sz) {
    return sz * 1024 * 1024 * 1024;
}

/// @brief Expands the value to tebibytes.
/// @param[in] sz the value to convert
/// @return sz * 1024 * 1024 * 1024 * 1024
inline constexpr unsigned long long operator""_TiB(unsigned long long sz) {
    return sz * 1024 * 1024 * 1024 * 1024;
}

namespace util {

/// @brief Reduces the value in bytes to kibibytes.
/// @param[in] sz the value to convert
/// @return sz / 1024.0
inline constexpr double BytesToKiB(unsigned long long sz) {
    return sz / 1024.0;
}

/// @brief Reduces the value in bytes to mebibytes.
/// @param[in] sz the value to convert
/// @return sz / 1024.0 / 1024.0
inline constexpr double BytesToMiB(unsigned long long sz) {
    return sz / 1024.0 / 1024.0;
}

/// @brief Reduces the value in bytes to gibibytes.
/// @param[in] sz the value to convert
/// @return sz / 1024.0 / 1024.0 / 1024.0
inline constexpr double BytesToGiB(unsigned long long sz) {
    return sz / 1024.0 / 1024.0 / 1024.0;
}

/// @brief Reduces the value in bytes to tebibytes.
/// @param[in] sz the value to convert
/// @return sz / 1024.0 / 1024.0 / 1024.0 / 1024.0
inline constexpr double BytesToTiB(unsigned long long sz) {
    return sz / 1024.0 / 1024.0 / 1024.0 / 1024.0;
}

} // namespace util
