# Bridge layer - adapts Ymir emulator to libretro API

add_library(brimir_bridge OBJECT
    core_wrapper.cpp
    input_manager.cpp
    audio_manager.cpp
    video_manager.cpp
    state_manager.cpp
)

target_include_directories(brimir_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_compile_features(brimir_bridge PUBLIC cxx_std_20)

# Link with Ymir (will be configured after examining Ymir's build system)
# target_link_libraries(brimir_bridge PRIVATE ymir_core)

