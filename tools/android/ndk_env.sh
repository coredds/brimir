# Finds the pinned Android NDK (tools/android/README.md). Sourced by build.sh and check_elf.sh.
#
# env:   ANDROID_NDK_HOME   the NDK, used when it is the version in tools/android/ndk-version.txt
#        ANDROID_SDK_ROOT or ANDROID_HOME   else the SDK's ndk/<pinned version> is used
#        BRIMIR_ALLOW_NDK_MISMATCH=1   accept another NDK version in ANDROID_NDK_HOME (never for releases)
#
# ndk_find sets NDK, NDK_SOURCE and NDK_VERSION (the pin); on failure it prints why and returns 1.

# Pkg.Revision of the NDK at $1, empty when it has none.
ndk_version() {
    sed -n 's/^Pkg.Revision *= *//p' "$1/source.properties" 2>/dev/null | tr -d '[:space:]' || true
}

# The NDK must be the pinned version: ANDROID_NDK_HOME when it is, else the SDK's ndk/<pin>. BRIMIR_ALLOW_NDK_MISMATCH=1
# accepts another version in ANDROID_NDK_HOME (local experiments only, never for a release build).
ndk_find() {
    local here sdk pin_ndk found
    here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
    NDK_VERSION=$(tr -d '[:space:]' < "$here/ndk-version.txt")
    sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
    pin_ndk=${sdk:+$sdk/ndk/$NDK_VERSION}

    NDK=""
    NDK_SOURCE=""
    if [ -n "${ANDROID_NDK_HOME:-}" ] && [ "$(ndk_version "$ANDROID_NDK_HOME")" = "$NDK_VERSION" ]; then
        NDK=$ANDROID_NDK_HOME
        NDK_SOURCE="ANDROID_NDK_HOME"
    elif [ -n "$pin_ndk" ] && [ "$(ndk_version "$pin_ndk")" = "$NDK_VERSION" ]; then
        NDK=$pin_ndk
        NDK_SOURCE="derived from the pin: \$ANDROID_SDK_ROOT (or \$ANDROID_HOME)/ndk/$NDK_VERSION"
        if [ -n "${ANDROID_NDK_HOME:-}" ]; then
            echo "note: ANDROID_NDK_HOME ($ANDROID_NDK_HOME) is version '$(ndk_version "$ANDROID_NDK_HOME")'," \
                 "not the pinned $NDK_VERSION; using $NDK" >&2
        fi
    elif [ -n "${ANDROID_NDK_HOME:-}" ] && [ "${BRIMIR_ALLOW_NDK_MISMATCH:-0}" = 1 ]; then
        NDK=$ANDROID_NDK_HOME
        NDK_SOURCE="ANDROID_NDK_HOME"
        found=$(ndk_version "$NDK")
        echo "warning: BRIMIR_ALLOW_NDK_MISMATCH=1: using NDK $NDK, version '${found:-unknown}'," \
             "pinned is $NDK_VERSION" >&2
    else
        echo "error: no Android NDK $NDK_VERSION (tools/android/ndk-version.txt); see tools/android/README.md" >&2
        if [ -n "${ANDROID_NDK_HOME:-}" ]; then
            found=$(ndk_version "$ANDROID_NDK_HOME")
            echo "  ANDROID_NDK_HOME=$ANDROID_NDK_HOME is version '${found:-unknown}'" \
                 "(set BRIMIR_ALLOW_NDK_MISMATCH=1 to use it anyway)" >&2
        else
            echo "  ANDROID_NDK_HOME is not set" >&2
        fi
        if [ -n "$pin_ndk" ]; then
            found=$(ndk_version "$pin_ndk")
            [ -n "$found" ] && found="version $found" || found="missing or not an NDK"
            echo "  $pin_ndk (derived from the pin under \$ANDROID_SDK_ROOT or \$ANDROID_HOME): $found" >&2
        else
            echo "  neither ANDROID_SDK_ROOT nor ANDROID_HOME is set" >&2
        fi
        return 1
    fi
}

# The NDK's LLVM tools (llvm-readelf, llvm-nm, clang); call after ndk_find.
ndk_llvm_bin() {
    echo "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
}
