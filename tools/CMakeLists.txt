# Headless frame benchmark: runs real content without a frontend and reports
# host ms/frame and the SH-2 share of emulation time. See tools/README.md.
add_executable(brimir_bench brimir_bench.cpp)

target_link_libraries(brimir_bench PRIVATE brimir_bridge brimir::brimir-core)

target_compile_features(brimir_bench PRIVATE cxx_std_20)
