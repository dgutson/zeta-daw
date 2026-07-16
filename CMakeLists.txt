cmake_minimum_required(VERSION 3.22)

project(zeta_daw LANGUAGES CXX)

option(ZETA_MIDI_TRACE "Trace routed live MIDI and loop playback events" OFF)
option(ZETA_GCC_ANALYZER "Enable GCC static analysis for production sources" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FLUIDSYNTH REQUIRED IMPORTED_TARGET fluidsynth)
find_package(ALSA REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)

find_package(libremidi 5.4.3 EXACT CONFIG QUIET)
if(NOT TARGET libremidi::libremidi)
    set(LIBREMIDI_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LIBREMIDI_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_JACK ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_PIPEWIRE ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_NETWORK ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_KEYBOARD ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_UDEV ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        libremidi
        URL https://github.com/celtera/libremidi/archive/refs/tags/v5.4.3.tar.gz
        URL_HASH SHA256=a1c369c6aab01c80f1506345d1c099a167b330d1d59159aa7583f7b0b9f966b9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(libremidi)
endif()

find_package(yaml-cpp CONFIG QUIET)
if(NOT TARGET yaml-cpp::yaml-cpp AND NOT TARGET yaml-cpp)
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
    zd
    main.cpp
    application.cpp
    configuration.cpp
    loop_slot.cpp
    loop_slot_fsm.cpp
    looper_fsm.cpp
    midi_control_change_mapping.cpp
    midi_event.cpp
    midi_input.cpp
    octave_transposer.cpp
    soundfont_selector.cpp
    synth_engine.cpp
)
target_link_libraries(
    zd
    PRIVATE
        PkgConfig::FLUIDSYNTH
        Threads::Threads
        ${ZETA_YAML_TARGET}
        libremidi::libremidi
)
target_compile_options(zd PRIVATE -Wall -Wextra -Wpedantic)
if(ZETA_GCC_ANALYZER)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "ZETA_GCC_ANALYZER requires GCC")
    endif()
    target_compile_options(
        zd
        PRIVATE
            -fanalyzer
            -Werror
            # GCC 13 reports this while constructing a standard unordered_map.
            -Wno-analyzer-malloc-leak
    )
endif()
if(ZETA_MIDI_TRACE)
    target_compile_definitions(zd PRIVATE ZETA_MIDI_TRACE)
endif()

include(CTest)

if(BUILD_TESTING)
    include(FetchContent)

    FetchContent_Declare(
        hegel
        URL https://github.com/hegeldev/hegel-cpp/archive/refs/tags/v0.7.1.tar.gz
        URL_HASH SHA256=d27dc8db6c1f9b9898e3d0ecd35844e4a65d059cfb093d91bbc20d4c1da2de2b
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(HEGEL_REFLECTION OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(hegel)

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
        tests/fake_midi_input.cpp
        application.cpp
        configuration.cpp
        loop_slot.cpp
        loop_slot_fsm.cpp
        looper_fsm.cpp
        midi_control_change_mapping.cpp
        midi_event.cpp
        midi_input.cpp
        octave_transposer.cpp
        soundfont_selector.cpp
        synth_engine.cpp
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
        PRIVATE
            GTest::gmock_main
            Threads::Threads
            ${ZETA_YAML_TARGET}
            libremidi::libremidi
    )

    add_executable(
        configuration_tests
        tests/configuration_test.cpp
        configuration.cpp
    )
    target_compile_options(configuration_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(
        configuration_tests
        PRIVATE GTest::gtest_main ${ZETA_YAML_TARGET} hegel
    )

    add_executable(
        loop_slot_fsm_tests
        tests/loop_slot_fsm_test.cpp
        loop_slot_fsm.cpp
    )
    target_compile_options(
        loop_slot_fsm_tests
        PRIVATE -Wall -Wextra -Wpedantic
    )
    target_link_libraries(
        loop_slot_fsm_tests
        PRIVATE GTest::gmock_main Threads::Threads hegel
    )

    add_executable(
        looper_fsm_tests
        tests/looper_fsm_test.cpp
        looper_fsm.cpp
        midi_event.cpp
    )
    target_compile_options(looper_fsm_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(looper_fsm_tests PRIVATE GTest::gmock_main Threads::Threads)

    add_executable(
        midi_control_change_mapping_tests
        tests/midi_control_change_mapping_test.cpp
        midi_control_change_mapping.cpp
    )
    target_compile_options(
        midi_control_change_mapping_tests
        PRIVATE -Wall -Wextra -Wpedantic
    )
    target_link_libraries(
        midi_control_change_mapping_tests
        PRIVATE GTest::gtest_main hegel
    )

    add_executable(
        midi_tests
        tests/midi_event_test.cpp
        midi_event.cpp
    )
    target_compile_options(midi_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(midi_tests PRIVATE GTest::gtest_main hegel)

    add_executable(
        soundfont_selector_tests
        tests/soundfont_selector_test.cpp
        soundfont_selector.cpp
    )
    target_compile_options(
        soundfont_selector_tests
        PRIVATE -Wall -Wextra -Wpedantic
    )
    target_link_libraries(
        soundfont_selector_tests
        PRIVATE GTest::gtest_main hegel
    )

    add_executable(
        octave_transposer_tests
        tests/octave_transposer_test.cpp
        octave_transposer.cpp
        midi_event.cpp
    )
    target_compile_options(
        octave_transposer_tests
        PRIVATE -Wall -Wextra -Wpedantic
    )
    target_link_libraries(
        octave_transposer_tests
        PRIVATE GTest::gtest_main hegel
    )

    include(GoogleTest)
    gtest_discover_tests(current_behavior_tests)
    gtest_discover_tests(configuration_tests)
    gtest_discover_tests(loop_slot_fsm_tests)
    gtest_discover_tests(looper_fsm_tests)
    gtest_discover_tests(midi_control_change_mapping_tests)
    gtest_discover_tests(midi_tests)
    gtest_discover_tests(soundfont_selector_tests)
    gtest_discover_tests(octave_transposer_tests)
endif()
