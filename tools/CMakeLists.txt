cmake_minimum_required(VERSION 3.21)

# SH-2 Microbenchmark Tool
add_executable(benchmark_sh2 benchmark_sh2.cpp)

target_link_libraries(benchmark_sh2 PRIVATE brimir::brimir-core)

target_compile_features(benchmark_sh2 PRIVATE cxx_std_20)

# Enable optimizations even in Debug
if(MSVC)
    target_compile_options(benchmark_sh2 PRIVATE /O2 /Oi)
else()
    target_compile_options(benchmark_sh2 PRIVATE -O2)
endif()

set_target_properties(benchmark_sh2 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}"
)

