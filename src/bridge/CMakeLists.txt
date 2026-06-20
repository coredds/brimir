# Bridge layer - adapts Ymir emulator to libretro API

add_library(brimir_bridge OBJECT
    core_wrapper.cpp
    stv/stv_cartridge.cpp
    stv/stv_315_5881.cpp
    stv/stv_io.cpp
    stv/stv_loader.cpp
    stv/stv_game_db.cpp
    stv/stv_zip.cpp
)

target_include_directories(brimir_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_compile_features(brimir_bridge PUBLIC cxx_std_20)

# Link with Brimir core
target_link_libraries(brimir_bridge PRIVATE brimir::brimir-core)
