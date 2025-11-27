# Selective inclusion of Ymir dependencies
# Only includes what ymir-core actually needs

message(STATUS "Adding Ymir dependencies (selective)")

# mio - memory mapped I/O
message(STATUS "==> mio")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/ymir/vendor/mio EXCLUDE_FROM_ALL)

# concurrentqueue - lock-free queue
message(STATUS "==> concurrentqueue")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/ymir/vendor/concurrentqueue EXCLUDE_FROM_ALL)

# xxHash - hashing library
message(STATUS "==> xxHash")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/ymir/vendor/xxHash EXCLUDE_FROM_ALL)

# lz4 - compression
message(STATUS "==> lz4")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/ymir/vendor/lz4 EXCLUDE_FROM_ALL)

# libchdr - CHD support
message(STATUS "==> libchdr")
set(BUILD_FUZZER OFF)
set(WITH_LZMA_ASM OFF CACHE BOOL "" FORCE)  # Disable lzma assembly to avoid MASM issues
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/ymir/vendor/libchdr EXCLUDE_FROM_ALL)

message(STATUS "Done adding Ymir dependencies")

