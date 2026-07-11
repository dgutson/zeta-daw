cmake_minimum_required(VERSION 3.20)

project(zeta_daw LANGUAGES CXX)

option(ZETA_MIDI_TRACE "Trace routed live MIDI and loop playback events" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FLUIDSYNTH REQUIRED IMPORTED_TARGET fluidsynth)
find_package(Threads REQUIRED)

find_package(yaml-cpp CONFIG QUIET)
if(NOT TARGET yaml-cpp::yaml-cpp AND NOT TARGET yaml-cpp)
    include(FetchContent)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        yaml-cpp
        URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(yaml-cpp)
endif()

if(TARGET yaml-cpp::yaml-cpp)
    set(ZETA_YAML_TARGET yaml-cpp::yaml-cpp)
else()
    set(ZETA_YAML_TARGET yaml-cpp)
endif()

add_executable(
    midi_looper
    midi2.cpp
    application.cpp
    configuration.cpp
    looper_fsm.cpp
)
target_link_libraries(
    midi_looper
    PRIVATE PkgConfig::FLUIDSYNTH Threads::Threads ${ZETA_YAML_TARGET}
)
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
        configuration.cpp
        looper_fsm.cpp
    )
    target_include_directories(
        current_behavior_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${FLUIDSYNTH_INCLUDE_DIRS}
    )
    target_compile_options(current_behavior_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(
        current_behavior_tests
        PRIVATE GTest::gmock_main Threads::Threads ${ZETA_YAML_TARGET}
    )

    add_executable(
        configuration_tests
        tests/configuration_test.cpp
        configuration.cpp
    )
    target_compile_options(configuration_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(
        configuration_tests
        PRIVATE GTest::gtest_main ${ZETA_YAML_TARGET}
    )

    add_executable(
        looper_fsm_tests
        tests/looper_fsm_test.cpp
        looper_fsm.cpp
    )
    target_compile_options(looper_fsm_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(looper_fsm_tests PRIVATE GTest::gmock_main Threads::Threads)

    include(GoogleTest)
    gtest_discover_tests(current_behavior_tests)
    gtest_discover_tests(configuration_tests)
    gtest_discover_tests(looper_fsm_tests)
endif()
