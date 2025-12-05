#pragma once

/**
@file
@brief Ymir library version definitions.
*/

#if BRIMIR_DEV_BUILD
    #ifdef BRIMIR_BUILD_TIMESTAMP
        #define BRIMIR_NIGHTLY_BUILD 1
    #else
        #define BRIMIR_NIGHTLY_BUILD 0
    #endif
#else
    #define BRIMIR_DEV_BUILD 0
    #define BRIMIR_NIGHTLY_BUILD 0
#endif

#define BRIMIR_STABLE_BUILD !BRIMIR_DEV_BUILD
#define BRIMIR_LOCAL_BUILD (BRIMIR_DEV_BUILD && !BRIMIR_NIGHTLY_BUILD)

namespace brimir::version {

/// @brief The library version string in the format "<major>.<minor>.<patch>[-<prerelease>][+<build>]".
///
/// The string follows the Semantic Versioning system, with mandatory major, minor and patch numbers and optional
/// prerelease and build components.
inline constexpr auto string = BRIMIR_VERSION;

inline constexpr auto major = static_cast<unsigned>(BRIMIR_VERSION_MAJOR); ///< The library's major version
inline constexpr auto minor = static_cast<unsigned>(BRIMIR_VERSION_MINOR); ///< The library's minor version
inline constexpr auto patch = static_cast<unsigned>(BRIMIR_VERSION_PATCH); ///< The library's patch version
inline constexpr auto prerelease = BRIMIR_VERSION_PRERELEASE;              ///< The library's prerelease version
inline constexpr auto build = BRIMIR_VERSION_BUILD;                        ///< The library's build version

/// @brief Whether this is a development build.
/// This is only ever `false` for stable releases.
inline constexpr bool is_dev_build = BRIMIR_DEV_BUILD;

/// @brief Whether this is a nightly build.
/// `false` means it's either a stable build or a local build.
inline constexpr bool is_nightly_build = BRIMIR_NIGHTLY_BUILD;

/// @brief Whether this is a stable build.
inline constexpr bool is_stable_build = BRIMIR_STABLE_BUILD;

/// @brief Whether this is a local build (neither stable nor nightly).
inline constexpr bool is_local_build = BRIMIR_LOCAL_BUILD;

} // namespace brimir::version
