# Brimir SH-2 JIT (see design/sh2-jit.md)
add_library(brimir-jit STATIC
    src/ir.cpp
    src/interp_backend.cpp
    src/sh2_helpers.cpp
    src/frontend.cpp
    src/block_cache.cpp
    src/executor.cpp
    src/backend.cpp
)

target_include_directories(brimir-jit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(brimir-jit PUBLIC cxx_std_20)
target_link_libraries(brimir-jit PUBLIC brimir::brimir-core)

# x64 native backend (design/sh2-jit-m2.md); BRIMIR_JIT_X64_EFFECTIVE is set by the root CMakeLists.
if(BRIMIR_JIT_X64_EFFECTIVE)
    target_sources(brimir-jit PRIVATE
        src/x64/x64_backend.cpp
        src/x64/x64_emitter.cpp
    )
    target_link_libraries(brimir-jit PRIVATE asmjit::asmjit)
    target_compile_definitions(brimir-jit PUBLIC BRIMIR_JIT_HAS_X64=1)
else()
    target_compile_definitions(brimir-jit PUBLIC BRIMIR_JIT_HAS_X64=0)
endif()
