# Core library dependencies
# These were originally from Ymir but are now maintained locally

message(STATUS "Adding core library dependencies")

# mio - memory mapped I/O
message(STATUS "==> mio")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/vendor/mio EXCLUDE_FROM_ALL)

# concurrentqueue - lock-free queue
message(STATUS "==> concurrentqueue")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/vendor/concurrentqueue EXCLUDE_FROM_ALL)

# xxHash - hashing library
message(STATUS "==> xxHash")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/vendor/xxHash EXCLUDE_FROM_ALL)

# lz4 - compression
message(STATUS "==> lz4")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/vendor/lz4 EXCLUDE_FROM_ALL)

# libchdr - CHD support
message(STATUS "==> libchdr")
set(BUILD_FUZZER OFF)
set(WITH_LZMA_ASM OFF CACHE BOOL "" FORCE)  # Disable lzma assembly to avoid MASM issues
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/vendor/libchdr EXCLUDE_FROM_ALL)

message(STATUS "Done adding core library dependencies")

