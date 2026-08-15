cmake_minimum_required(VERSION 3.22)

project(zeta_daw LANGUAGES C CXX)

option(ZETA_MIDI_TRACE "Trace routed live MIDI and loop playback events" OFF)
option(ZETA_GCC_ANALYZER "Enable GCC static analysis for production sources" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Deployment builds run only on the machine that built them. Let the compiler
# use that host's complete instruction set for both Zeta and FluidSynth.
string(APPEND CMAKE_C_FLAGS_RELEASE " -march=native")
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -march=native")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ALSA REQUIRED)
find_package(OpenMP REQUIRED COMPONENTS C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSEAUDIO REQUIRED IMPORTED_TARGET libpulse-simple)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
find_package(Threads REQUIRED)

include(FetchContent)
include(ExternalProject)

set(ZETA_FLUIDSYNTH_VERSION 2.5.7)
FetchContent_Declare(
    zeta_fluidsynth_source
    URL
        https://github.com/FluidSynth/fluidsynth/archive/refs/tags/v${ZETA_FLUIDSYNTH_VERSION}.tar.gz
    URL_HASH
        SHA256=ce27840221ab00dd59bf27e85ecbba480c6c2a7c9fbec4243658f68f59c07f4a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(zeta_fluidsynth_source)
if(NOT zeta_fluidsynth_source_POPULATED)
    FetchContent_Populate(zeta_fluidsynth_source)
endif()

FetchContent_Declare(
    zeta_gcem_source
    URL
        https://github.com/kthohr/gcem/archive/012ae73c6d0a2cb09ffe86475f5c6fba3926e200.zip
    URL_HASH
        SHA256=28159274c54e9640354852e172d10d88eb159f4e7f2fea42edbcd20105ed3526
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(zeta_gcem_source)
if(NOT zeta_gcem_source_POPULATED)
    FetchContent_Populate(zeta_gcem_source)
endif()

set(ZETA_FLUIDSYNTH_SOURCE_DIR ${zeta_fluidsynth_source_SOURCE_DIR})
set(
    ZETA_FLUIDSYNTH_BINARY_DIR
    ${CMAKE_BINARY_DIR}/_deps/fluidsynth-build
)
set(
    ZETA_FLUIDSYNTH_LIBRARY
    ${ZETA_FLUIDSYNTH_BINARY_DIR}/src/libfluidsynth.a
)
file(MAKE_DIRECTORY "${ZETA_FLUIDSYNTH_BINARY_DIR}/include")

set(ZETA_FLUIDSYNTH_RELEASE_CMAKE_ARGS)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    list(
        APPEND ZETA_FLUIDSYNTH_RELEASE_CMAKE_ARGS
        "-DCMAKE_C_FLAGS_RELEASE:STRING=${CMAKE_C_FLAGS_RELEASE}"
        "-DCMAKE_CXX_FLAGS_RELEASE:STRING=${CMAKE_CXX_FLAGS_RELEASE}"
    )
endif()

# FluidSynth registers its own tests unconditionally when added as a
# subdirectory. Keep its build isolated so Zeta's test suite contains only
# Zeta tests, and link it statically to preserve the single-binary install.
ExternalProject_Add(
    zeta_fluidsynth_external
    DOWNLOAD_COMMAND ""
    SOURCE_DIR ${ZETA_FLUIDSYNTH_SOURCE_DIR}
    BINARY_DIR ${ZETA_FLUIDSYNTH_BINARY_DIR}
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=OFF
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        ${ZETA_FLUIDSYNTH_RELEASE_CMAKE_ARGS}
        -DGCEM_INCLUDE_DIR=${zeta_gcem_source_SOURCE_DIR}/include
        -DPROJECT_WC_REVISION=v${ZETA_FLUIDSYNTH_VERSION}
        -Denable-alsa=ON
        -Denable-aufile=OFF
        -Denable-dbus=OFF
        -Denable-jack=OFF
        -Denable-ladspa=OFF
        -Denable-libinstpatch=OFF
        -Denable-libsndfile=ON
        -Denable-midishare=OFF
        -Denable-native-dls=OFF
        -Denable-network=OFF
        -Denable-openmp=ON
        -Denable-oss=OFF
        -Denable-pipewire=OFF
        -Denable-pulseaudio=ON
        -Denable-readline=OFF
        -Denable-sdl3=OFF
        -Denable-systemd=OFF
        -Denable-threads=ON
        -Dosal=cpp11
    BUILD_COMMAND
        ${CMAKE_COMMAND}
        --build <BINARY_DIR>
        --target libfluidsynth
        --parallel
    BUILD_BYPRODUCTS ${ZETA_FLUIDSYNTH_LIBRARY}
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)

add_library(zeta_fluidsynth STATIC IMPORTED GLOBAL)
set_target_properties(
    zeta_fluidsynth
    PROPERTIES
        IMPORTED_LOCATION ${ZETA_FLUIDSYNTH_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES
            "${ZETA_FLUIDSYNTH_BINARY_DIR}/include;${ZETA_FLUIDSYNTH_SOURCE_DIR}/include"
)
target_link_libraries(
    zeta_fluidsynth
    INTERFACE
        ALSA::ALSA
        OpenMP::OpenMP_C
        PkgConfig::PULSEAUDIO
        PkgConfig::SNDFILE
        Threads::Threads
        ${CMAKE_DL_LIBS}
        m
)
add_dependencies(zeta_fluidsynth zeta_fluidsynth_external)

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

    add_subdirectory(cmake/yaml_cpp_fallback)
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
    loop_slot_group.cpp
    loop_timing.cpp
    looper_fsm.cpp
    midi_control_change_mapping.cpp
    midi_event.cpp
    midi_input.cpp
    octave_transposer.cpp
    pending_take.cpp
    soundfont_selector.cpp
    synth_engine.cpp
)
target_link_libraries(
    zd
    PRIVATE
        zeta_fluidsynth
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

add_executable(zfont zfont.cpp configuration.cpp)
target_link_libraries(
    zfont
    PRIVATE
        zeta_fluidsynth
        ${ZETA_YAML_TARGET}
)
target_compile_options(zfont PRIVATE -Wall -Wextra -Wpedantic)

add_executable(zsoundtest sound_test.cpp configuration.cpp)
target_link_libraries(
    zsoundtest
    PRIVATE
        zeta_fluidsynth
        ${ZETA_YAML_TARGET}
)
target_compile_options(zsoundtest PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)

if(BUILD_TESTING)
    include(FetchContent)

    FetchContent_Declare(
        hegel
        URL https://github.com/hegeldev/hegel-cpp/archive/refs/tags/v0.7.4.tar.gz
        URL_HASH SHA256=690d5dfb6517245ab5f162e9f0cc59739d709848bb969a2afa725b92f50bfcd3
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    # Keep a reused build directory from retaining Hegel's ABI-incompatible
    # native engine pin from an earlier release.
    set(
        HEGEL_LIBHEGEL_VERSION
        0.29.0
        CACHE STRING "libhegel version required by Hegel C++ v0.7.4"
        FORCE
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
        loop_slot_group.cpp
        loop_timing.cpp
        looper_fsm.cpp
        midi_control_change_mapping.cpp
        midi_event.cpp
        midi_input.cpp
        octave_transposer.cpp
        pending_take.cpp
        soundfont_selector.cpp
        synth_engine.cpp
    )
    target_include_directories(
        current_behavior_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${ZETA_FLUIDSYNTH_BINARY_DIR}/include
            ${ZETA_FLUIDSYNTH_SOURCE_DIR}/include
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
    add_dependencies(current_behavior_tests zeta_fluidsynth_external)

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
        looper_fsm_tests
        tests/looper_fsm_test.cpp
        loop_timing.cpp
        looper_fsm.cpp
        midi_event.cpp
    )
    target_compile_options(looper_fsm_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(looper_fsm_tests PRIVATE GTest::gmock_main Threads::Threads)

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
        PRIVATE
            GTest::gmock_main
            Threads::Threads
            hegel
    )

    add_executable(
        loop_timing_tests
        tests/loop_timing_test.cpp
        loop_timing.cpp
    )
    target_compile_options(
        loop_timing_tests
        PRIVATE -Wall -Wextra -Wpedantic
    )
    target_link_libraries(
        loop_timing_tests
        PRIVATE GTest::gtest_main hegel
    )

    add_executable(
        pending_take_tests
        tests/pending_take_test.cpp
        pending_take.cpp
    )
    target_compile_options(
        pending_take_tests
        PRIVATE -Wall -Wextra -Wpedantic
    )
    target_link_libraries(pending_take_tests PRIVATE GTest::gtest_main)

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
    gtest_discover_tests(looper_fsm_tests)
    gtest_discover_tests(loop_slot_fsm_tests)
    gtest_discover_tests(loop_timing_tests)
    gtest_discover_tests(pending_take_tests)
    gtest_discover_tests(midi_control_change_mapping_tests)
    gtest_discover_tests(midi_tests)
    gtest_discover_tests(soundfont_selector_tests)
    gtest_discover_tests(octave_transposer_tests)
endif()
