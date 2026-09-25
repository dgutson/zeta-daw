# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Zeta DAW is a Linux-only C++20 MIDI looper that runs on a desktop or as a
headless Raspberry Pi 5 service and is operated only from a MIDI controller.

## Project documents

- `CONTRIBUTING.md` is the architectural contract: product constraints, what
  each source file owns, the GoF State FSM rules, state semantics, the
  configuration schema policy, concurrency, coding rules, the purpose of each
  test suite, and the "Before submitting" checklist. Where it and this file
  differ, follow `CONTRIBUTING.md`.
- `HEGEL.md` covers when and how to write Hegel property tests.
- `README.md` covers configuration, the `zfont` and `zsoundtest` diagnostics,
  and Raspberry Pi deployment.
- `runtime-sequence.puml` shows startup, MIDI routing, recording, playback
  workers, and shutdown as one sequence diagram.
- `CHANGELOG.md` gets an entry under `Unreleased` in every change.

## Commands

`build/` holds whichever of three configurations ran last: `./build.sh`
(Release, no tests), the test configuration, or the clang-tidy configuration.
Each one replaces the others' cached settings; for example, `./build.sh` turns
the tests off and rebuilds `build/zd` as a Release binary.

```bash
./build.sh         # Release build/zd, zfont, zsoundtest; -march=native, so build on the machine that runs it
./build_debug.sh   # build-debug/zd with MIDI routing traces (ZETA_MIDI_TRACE=ON)
```

A documentation-only change (prose, diagrams, the changelog, non-executable
example configuration) needs only `git diff --check`. Every other change needs
the full suite as well:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
git diff --check
```

CTest names each test `Suite.Name`, not by executable, so select tests with a
regular expression over those names, or run an executable directly:

```bash
ctest --test-dir build -R '^LoopTimingTest\.' --output-on-failure   # one suite
ctest --test-dir build -R PropertyTest --output-on-failure            # every Hegel property
./build/loop_timing_tests --gtest_filter='LoopTimingTest.*'           # one executable
```

Static analysis as CI runs it, over production sources only:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --target zeta_fluidsynth_external --parallel   # generated FluidSynth headers
run-clang-tidy-18 -p build -j 2 "$(pwd)/(?!zfont\.cpp$)[^/]+\.cpp$"

cmake -S . -B build-analyzer -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF -DZETA_GCC_ANALYZER=ON
cmake --build build-analyzer --parallel
```

CMake downloads and statically links a pinned FluidSynth (never the
distribution's), uses installed libremidi and yaml-cpp or downloads pinned
copies, and downloads GoogleTest and Hegel for test builds, so the first
configure needs network access.

## Code structure

Sources are flat in the repository root, one `name.hpp`/`name.cpp` pair per
owner. A MIDI event is handled in this order:

1. `midi_input.cpp` receives it from libremidi, decodes it with
   `decodeMidiEvent`, applies that port's Control Change mapping, and calls the
   handler `Application` registered.
2. `Application::handleMidiEvent` checks the configured controls in turn; a
   match becomes an FSM stimulus such as `loopSlotControlPressed`, and any
   other event becomes `midiMessage`.
3. `LooperFsm` takes its mutex, calls the stimulus on the current `LooperState`
   object, and installs the `StateId` it returns.
4. State objects cause effects only through `LooperOutput`, which
   `Application` implements by driving `SynthEngine` (live output on channel 0)
   and `LoopSlotGroup`, whose slot N plays on channel N+1 from its own worker
   thread.

Facts that involve several files:

- CMake lists sources per target. `current_behavior_tests` compiles every
  production source except `main.cpp`, and each unit-test executable compiles
  only its subject and that subject's dependencies, so a new production `.cpp`
  goes into `zd`, `current_behavior_tests`, and any unit-test target that uses
  it. `zfont` and `zsoundtest` compile `configuration.cpp`, so configuration
  changes must keep them building too.
- Tests replace MIDI input through the constructor
  `Application(ApplicationConfig, std::unique_ptr<MidiInput>)`, using
  `tests/fake_midi_input.*`. They replace FluidSynth at link time instead:
  `current_behavior_tests` compiles the real `synth_engine.cpp` without linking
  FluidSynth, and `tests/fake_fluidsynth.cpp` defines the `fluid_*` functions
  it calls and records each call for assertions.
- Hegel is linked only into the test executables that contain properties,
  never into `zd`.
