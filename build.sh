#!/bin/bash
# Brimir - Linux Build Script
# Copyright (C) 2025 coredds
# Licensed under GPL-3.0

set -e  # Exit on error

echo "=================================================="
echo "Brimir Core - Linux Build Script"
echo "=================================================="
echo ""

# Configuration
BUILD_DIR="build-linux"
BUILD_TYPE="${BUILD_TYPE:-Release}"
ENABLE_TESTS="${ENABLE_TESTS:-OFF}"
NUM_JOBS="${NUM_JOBS:-$(nproc)}"
USE_CLANG="${USE_CLANG:-YES}"  # Use Clang by default (required for force-inline optimizations)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Helper functions
print_info() {
    echo -e "${CYAN}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[OK]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check for required tools
print_info "Checking for required tools..."

if ! command -v cmake &> /dev/null; then
    print_error "CMake not found!"
    echo "Install with: sudo apt install cmake (Ubuntu/Debian)"
    echo "             sudo dnf install cmake (Fedora)"
    echo "             sudo pacman -S cmake (Arch)"
    exit 1
fi

if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    print_error "No C++ compiler found!"
    echo "Install with: sudo apt install build-essential clang (Ubuntu/Debian)"
    echo "             sudo dnf install gcc-c++ clang (Fedora)"
    echo "             sudo pacman -S base-devel clang (Arch)"
    exit 1
fi

# Require Clang for force-inline optimizations (matches Windows MSVC behavior)
if [ "$USE_CLANG" = "YES" ]; then
    if command -v clang++ &> /dev/null; then
        export CC=clang
        export CXX=clang++
        print_success "Using Clang compiler"
    else
        print_error "Clang not found but required for optimal performance"
        echo "Install Clang with:"
        echo "  Ubuntu/Debian: sudo apt install clang"
        echo "  Fedora:        sudo dnf install clang"
        echo "  Arch:          sudo pacman -S clang"
        echo ""
        echo "To use GCC instead (not recommended, slower): USE_CLANG=NO ./build.sh"
        exit 1
    fi
else
    export CC=gcc
    export CXX=g++
    print_warning "Using GCC compiler - force-inline optimizations disabled"
    print_warning "Performance may be reduced. Use Clang for best results."
fi

print_success "All required tools found"

# Check CMake version
CMAKE_VERSION=$(cmake --version | head -n1 | cut -d' ' -f3)
print_info "CMake version: $CMAKE_VERSION"

# Bootstrap vcpkg if needed
print_info "Checking vcpkg..."

if [ ! -f "vcpkg/vcpkg" ]; then
    print_info "Bootstrapping vcpkg..."
    if [ -f "vcpkg/bootstrap-vcpkg.sh" ]; then
        cd vcpkg
        ./bootstrap-vcpkg.sh
        cd ..
        print_success "vcpkg bootstrapped"
    else
        print_error "vcpkg bootstrap script not found!"
        exit 1
    fi
else
    print_success "vcpkg already bootstrapped"
fi

# Install dependencies via vcpkg
print_info "Installing dependencies..."

if [ "$ENABLE_TESTS" = "ON" ]; then
    ./vcpkg/vcpkg install fmt:x64-linux catch2:x64-linux
else
    ./vcpkg/vcpkg install fmt:x64-linux
fi

print_success "Dependencies installed"

# Configure
print_info "Configuring build (Type: $BUILD_TYPE)..."

cmake -B "$BUILD_DIR" -S . \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DBRIMIR_BUILD_TESTS="$ENABLE_TESTS" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

print_success "Configuration complete"

# Build
print_info "Building Brimir core (using $NUM_JOBS jobs)..."

# Note: Makefiles don't use --config, only multi-config generators like VS do
cmake --build "$BUILD_DIR" --parallel "$NUM_JOBS"

print_success "Build complete"

# Display output
echo ""
echo "=================================================="
print_success "Build Summary"
echo "=================================================="
echo ""
echo "Build Type:     $BUILD_TYPE"
echo "Build Directory: $BUILD_DIR"
echo "Output:"
echo "  - Core library: $BUILD_DIR/lib/brimir_libretro.so"
if [ "$ENABLE_TESTS" = "ON" ]; then
    echo "  - Unit tests:   $BUILD_DIR/tests/unit/"
fi
echo ""

# Run tests if enabled
if [ "$ENABLE_TESTS" = "ON" ]; then
    print_info "Running tests..."
    cd "$BUILD_DIR"
    ctest -C "$BUILD_TYPE" --output-on-failure
    cd ..
    print_success "All tests passed"
fi

echo ""
print_success "Build process complete!"
echo ""
echo "Next steps:"
echo "  - Install: sudo make -C build-linux install"
echo "  - Or copy: cp build-linux/lib/brimir_libretro.so ~/.config/retroarch/cores/"
echo ""

