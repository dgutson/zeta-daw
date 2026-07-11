cmake_minimum_required(VERSION 3.20)

project(zeta_daw LANGUAGES CXX)

option(ZETA_MIDI_TRACE "Trace routed live MIDI and loop playback events" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FLUIDSYNTH REQUIRED IMPORTED_TARGET fluidsynth)
find_package(Threads REQUIRED)

add_executable(
    midi_looper
    midi2.cpp
    application.cpp
    looper_fsm.cpp
)
target_link_libraries(midi_looper PRIVATE PkgConfig::FLUIDSYNTH Threads::Threads)
target_compile_options(midi_looper PRIVATE -Wall -Wextra -Wpedantic)
if(ZETA_MIDI_TRACE)
    target_compile_definitions(midi_looper PRIVATE ZETA_MIDI_TRACE)
endif()

include(CTest)

if(BUILD_TESTING)
    include(FetchContent)

    # Pin the latest GoogleTest release available when this suite was added.
    # A release archive keeps test builds reproducible.
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(
        current_behavior_tests
        tests/current_behavior_test.cpp
        tests/fake_fluidsynth.cpp
        application.cpp
        looper_fsm.cpp
    )
    target_include_directories(
        current_behavior_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${FLUIDSYNTH_INCLUDE_DIRS}
    )
    target_compile_options(current_behavior_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(current_behavior_tests PRIVATE GTest::gmock_main Threads::Threads)

    add_executable(
        looper_fsm_tests
        tests/looper_fsm_test.cpp
        looper_fsm.cpp
    )
    target_compile_options(looper_fsm_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(looper_fsm_tests PRIVATE GTest::gmock_main Threads::Threads)

    include(GoogleTest)
    gtest_discover_tests(current_behavior_tests)
    gtest_discover_tests(looper_fsm_tests)
endif()
