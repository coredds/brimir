# Brimir Tests

find_package(Catch2 3 CONFIG REQUIRED)

# Test executable
add_executable(brimir_tests
    main.cpp
    unit/test_core_wrapper.cpp
    unit/test_video_output.cpp
    unit/test_audio_output.cpp
    unit/test_input_handling.cpp
    unit/test_bios.cpp
    unit/test_bios_integration.cpp
    unit/test_game_loading.cpp
    unit/test_save_states.cpp
    unit/test_cartridge_support.cpp
    unit/test_sram_persistence.cpp
)

target_link_libraries(brimir_tests PRIVATE
    brimir_bridge
    ymir::ymir-core
    Catch2::Catch2WithMain
)

target_include_directories(brimir_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# Discover tests for CTest
include(CTest)
include(Catch)
catch_discover_tests(brimir_tests)

# Copy test fixtures if they exist
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
         DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()


