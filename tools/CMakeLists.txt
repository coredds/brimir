# Headless frame benchmark: runs real content without a frontend and reports
# host ms/frame and the SH-2 share of emulation time. See tools/README.md.
add_executable(brimir_bench brimir_bench.cpp)

target_link_libraries(brimir_bench PRIVATE brimir_bridge brimir::brimir-core)

target_compile_features(brimir_bench PRIVATE cxx_std_20)

# Headless libretro host: loads brimir_libretro dynamically and records its frames (tools/README.md). Built with
# BRIMIR_VULKAN because its Vulkan context provider (hardware frames) lives here.
if(BRIMIR_VULKAN)
    add_executable(brimir_vkhost
        vkhost/main.cpp
        vkhost/host.cpp
        vkhost/run_core.cpp
    )
    target_include_directories(brimir_vkhost PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_features(brimir_vkhost PRIVATE cxx_std_20)
    if(UNIX)
        target_link_libraries(brimir_vkhost PRIVATE ${CMAKE_DL_LIBS})
    endif()
    # The core is loaded at runtime, not linked: build it alongside.
    add_dependencies(brimir_vkhost brimir_libretro)
endif()
