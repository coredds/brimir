#!/bin/bash
# Brimir Build Script (Linux)

set -e

BUILD_TYPE="Release"
CLEAN=false
RECONFIGURE=false
GPU=true
JOBS=$(nproc 2>/dev/null || echo 4)

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)        BUILD_TYPE="Debug"; shift ;;
        --release)      BUILD_TYPE="Release"; shift ;;
        --clean)        CLEAN=true; shift ;;
        --reconfigure)  RECONFIGURE=true; shift ;;
        --no-gpu)       GPU=false; shift ;;
        --jobs|-j)      JOBS="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: ./build.sh [options]"
            echo ""
            echo "Options:"
            echo "  --debug          Build in Debug mode"
            echo "  --release        Build in Release mode (default)"
            echo "  --clean          Clean build directory before building"
            echo "  --reconfigure    Force CMake reconfiguration"
            echo "  --no-gpu         Disable Vulkan GPU renderer (enabled by default)"
            echo "  --jobs, -j N     Number of parallel build jobs (default: nproc)"
            echo "  --help, -h       Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo ""
echo "========================================"
echo "    Brimir Build Script (Linux)"
echo "========================================"
echo ""

# Clean build directory if requested
if [ "$CLEAN" = true ]; then
    echo "Cleaning build directory..."
    rm -rf build
    echo "Build directory cleaned."
    RECONFIGURE=true
fi

# Configure CMake if needed
if [ "$RECONFIGURE" = true ] || [ ! -f "build/CMakeCache.txt" ]; then
    echo ""
    echo "Configuring CMake..."
    echo "  Build Type: $BUILD_TYPE"
    echo "  GPU: $([ "$GPU" = true ] && echo 'Enabled (Vulkan)' || echo 'Disabled')"
    echo ""

    CMAKE_ARGS="-B build -G Ninja -DCMAKE_BUILD_TYPE=$BUILD_TYPE"

    if [ "$GPU" = true ]; then
        CMAKE_ARGS="$CMAKE_ARGS -DBRIMIR_GPU_VULKAN=ON"
    fi

    cmake $CMAKE_ARGS

    if [ $? -ne 0 ]; then
        echo ""
        echo "ERROR: CMake configuration failed!"
        exit 1
    fi

    echo "Configuration complete."
fi

# Build
echo ""
echo "Building brimir_libretro ($BUILD_TYPE)..."
echo ""

cmake --build build --config "$BUILD_TYPE" --target brimir_libretro --parallel "$JOBS"

if [ $? -ne 0 ]; then
    echo ""
    echo "ERROR: Build failed!"
    exit 1
fi

echo ""
echo "========================================"
echo "    BUILD SUCCESSFUL!"
echo "========================================"
echo ""

# Find and display output
OUTPUT_PATHS=(
    "build/bin/brimir_libretro.so"
    "build/lib/brimir_libretro.so"
)

for path in "${OUTPUT_PATHS[@]}"; do
    if [ -f "$path" ]; then
        SIZE=$(du -h "$path" | cut -f1)
        echo "Output: $path ($SIZE)"
        break
    fi
done

echo ""
