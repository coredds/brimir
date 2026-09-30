# Brimir SH-2 JIT (see design/sh2-jit.md)
add_library(brimir-jit STATIC
    src/ir.cpp
)

target_include_directories(brimir-jit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(brimir-jit PUBLIC cxx_std_20)
target_link_libraries(brimir-jit PUBLIC brimir::brimir-core)
