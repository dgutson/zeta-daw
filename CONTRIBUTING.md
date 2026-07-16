# Contributing to Zeta DAW

This document is the architectural contract for the repository. Contributors
and coding assistants must read it completely before proposing or making a
change. Update it when an agreed architectural decision changes; do not let a
refactor silently redefine the design.

## Product constraints

Zeta is a live-performance instrument, not a general-purpose sequencer. Design
decisions must preserve these constraints:

- The primary target is a Raspberry Pi 5 that boots headlessly and is operated
  only from a MIDI controller. The same executable must remain usable as a
  normal Linux desktop process.
- Startup and shutdown are unattended. SIGINT and SIGTERM must stop playback,
  silence notes, release MIDI/audio resources, and terminate cleanly.
- Performance-time operations must be deterministic and low latency. All
  configured SoundFonts are loaded eagerly before MIDI input becomes active;
  do not introduce lazy loading on a performance path.
- Do not sacrifice any playable piano note to solve a controller-integration
  problem. Note bindings remain available as a generic user option, but they
  are not an acceptable built-in solution for the SE49 or the default setup.
- Sustain must remain ordinary CC64 input unless the user explicitly chooses
  to bind it. A feature must not consume or emulate sustain incidentally.
- Preserve expressive MIDI behavior. Arithmetic octave transposition is the
  deliberate exception to otherwise avoiding note-stream rewriting; keep that
  exception inside the project-owned octave transposer rather than FluidSynth.
- The first positive-velocity Note On after arming starts the take at offset
  zero. This deliberate behavior removes leading silence and must not regress.
- SoundFonts needed on stage are selected in configuration before the
  performance. Runtime selection chooses among that bounded, preloaded list.
- Controller-specific hardware and documentation may inspire workflows,
  examples, and naming conventions, but must not become core restrictions
  unless explicitly agreed. Configuration and runtime behavior must remain
  usable with equivalent MIDI controllers.

## Architecture at a glance

The runtime flow is:

```text
libremidi/ALSA input
        |
        v
project-owned MidiEvent decoding, per-port CC mapping,
and configured-control matching
        |
        v
LooperFsm -> current LooperState -> LooperOutput
                                      |
                                      v
                                  Application
                                  /         \
                         SynthEngine      loop worker
                         (FluidSynth)
```

The main layers and ownership boundaries are:

- `main.cpp` loads configuration, owns process signal handling, constructs the
  application, and waits for termination.
- `configuration.*` owns the strict YAML schema and converts it to
  project-owned configuration types.
- `midi_control_change_mapping.*` owns exact, dependency-free Control Change
  mapping definitions and immutable per-port lookup state.
- `midi_input.*` owns hardware MIDI discovery, connection, disconnection, and
  reconnection through libremidi's ALSA Sequencer backend. It associates
  configured mappings with a source port while the port name is available.
- `midi_event.*` owns the library-independent MIDI event model and raw-byte
  decoding, including channel-message data-byte validation and MMC SysEx
  recognition.
- `looper_fsm.*` owns state-dependent decisions. It does not own FluidSynth,
  libremidi, configuration parsing, or the playback thread.
- `application.*` composes the system, matches configured controls, implements
  the FSM output alphabet, stores a take, and owns the loop worker.
- `octave_transposer.*` owns octave bounds and arithmetic transposition for one
  MIDI route. `Application` owns independent live and loop instances.
- `soundfont_selector.*` owns the ordered SoundFont selection, its current
  entry, and the bounded physical-key lookup shared by sequential and direct
  selection. It has no synthesizer or MIDI-channel responsibility.
- `synth_engine.*` is the narrow RAII wrapper around the FluidSynth C API.

Keep dependency-specific types at their adapters. libremidi types must not
leak past `midi_input.*`, and FluidSynth types must not leak past
`synth_engine.*`.

## FSM design contract

The looper deliberately uses a GoF State pattern. Preserve that style.

- Each state is a polymorphic `LooperState` implementation. `LooperFsm` holds a
  `StateId` and dispatches stimuli to the object supplied by
  `LooperStateRegistry`.
- A performer-visible interaction phase that changes how later stimuli are
  interpreted is an FSM state. Represent it with an explicit `StateId` and
  `LooperState` implementation; do not encode it as a boolean, enum, or other
  hidden mode in `LooperStateData`, `Application`, or an adapter.
- `LooperStateData` stores values used by explicit states and transitions, such
  as the recording start time. It must not duplicate the current `StateId` or
  act as a second, implicit state machine.
- Do not replace this design with `std::variant`, visitation, a switch-based
  state machine, function tables, or a third-party FSM framework.
- The virtual methods on `LooperState` are the FSM input alphabet: currently
  `recordingControlPressed`, `nextSoundFontPressed`,
  `soundFontByNotePressed`, `octaveDownPressed`, `octaveUpPressed`,
  `midiMessage`, and `shutdownRequested`. Add a virtual method when an agreed
  feature introduces a genuinely new stimulus.
- The virtual methods on `LooperOutput` are the output alphabet. State objects
  request effects through this interface; they must not reach into
  `Application`, `SynthEngine`, libremidi, or FluidSynth.
- `LooperOutput&` and `LooperStateData&` are invariant for the FSM lifetime and
  are constructor-injected into the `LooperState` base. Do not pass them
  repeatedly through stimulus methods or replace them with setters.
- A stimulus should take only event-specific information. A button stimulus
  such as `nextSoundFontPressed()` needs no argument. Persistent transition
  data belongs in `LooperStateData`; effects belong in `LooperOutput`.
- State methods return the next `StateId`. `midiMessage` returns
  `MidiHandlingResult` because it must also propagate the synthesizer's native
  result. Do not generalize that special case without a concrete need.
- `LooperFsm` serializes stimuli and access to the registry-owned shared state
  data with its mutex, and validates every installed `StateId` through the
  registry. Keep state objects free of independent mutable lifecycle state.
- Common state behavior belongs in a shared state base or a small helper when
  that is clearer and DRYer than repetition.

Any proposal that changes these boundaries or the GoF model must be discussed
and explicitly agreed before code is changed. Do not hide an architectural
rewrite inside a feature or cleanup ticket.

## Current state semantics

The states are `Ready`, `ReadySelectingSoundFont`, `Armed`,
`ArmedSelectingSoundFont`, `Recording`, `Looping`,
`LoopingSelectingSoundFont`, and `Stopped`.

- `Ready`: MIDI is monitored on the live channel. Primary control arms a new
  take and assigns the current SoundFont to the loop channel. Next changes the
  live SoundFont. Octave controls change both live and loop transposition.
- `Armed`: MIDI is monitored on the loop channel. The first positive-velocity
  Note On enters `Recording` and is stored at offset zero. Next may change the
  pending loop SoundFont before that first note. Octave controls change both
  live and loop transposition. Primary control before that first note cancels
  the pending take, adopts its SoundFont for live playing, and returns to
  `Ready`.
- `Recording`: Note On and Note Off events are timestamped and stored. Next is
  consumed without effect, as are both octave controls. Primary control commits
  the duration and starts loop playback.
- `Looping`: recorded notes repeat on the loop channel while incoming MIDI is
  monitored on the live channel. Next changes only the live SoundFont. Octave
  controls change only live transposition. Primary control stops playback and
  returns to `Ready` without stopping the playback worker.
- `Stopped`: all stimuli are inert and the application terminates. Only the
  shutdown stimulus enters this state.

`ReadySelectingSoundFont`, `ArmedSelectingSoundFont`, and
`LoopingSelectingSoundFont` are explicit one-shot interaction states. The next
positive-velocity Note On is consumed, selects the configured SoundFont for
the originating state's route by its raw physical key regardless of incoming
MIDI channel, and returns to that originating state. An
unmapped note is consumed and returns without changing the selection. Pressing
the selector control again cancels. Other MIDI retains the originating route
and selection state; other configured actions perform their ordinary behavior
and leave the selection state. Armed selection does not sound a note or start
recording. Recording ignores the selector control.

The same configured primary control arms, finishes, and cancels recording, and
stops an active loop. Controller stimuli never terminate the application;
SIGINT, SIGTERM, and destruction use the shutdown stimulus for that lifecycle
transition.

Live and loop output use fixed FluidSynth channels 0 and 1 respectively.
Recorded notes retain their original velocity and store the key produced by
the pending loop transposer. They play on the loop channel. Once recording
begins, the loop channel's SoundFont and octave are locked. When a take starts
looping, live output is synchronized to the current selection; later Next
events affect only live output.

Octave transposition starts at zero, moves in twelve-semitone steps, and clamps
from three octaves down through four octaves up. The performer changes octaves
only while no notes are playing. Do not add held-note tracking or behavior for
octave changes during active notes without changing this contract explicitly.
Live and loop transposition start synchronized for the first take. Returning
from `Looping` to `Ready` preserves their independent octave selections, and
subsequent Ready octave controls move both from those preserved values. A later
take therefore uses the preserved loop selection rather than synchronizing it
to the live selection.
Key-bearing messages whose shifted key would fall outside MIDI range 0 through
127 are left unchanged. Recorded keys are transposed before storage, so the
playback worker does not share octave state and the loop pitch stays locked.

Source-port Control Change mappings are applied once after decoding and before
configured application actions are matched. They replace only the controller
number and preserve the MIDI channel and value. Control events are then matched
before ordinary MIDI reaches the FSM. A matched event is consumed, so it is
neither synthesized nor recorded. Configuration must reject bindings that make
any application actions ambiguous.

## libremidi and FluidSynth are both intentional

These libraries have different responsibilities and neither replaces the
other:

- libremidi is the MIDI input layer. It uses ALSA Sequencer, observes hardware
  ports, handles hotplug/reconnection, receives SysEx, and invokes the
  project-owned event callback.
- FluidSynth is the synthesis and audio layer. `SynthEngine` loads SoundFonts,
  selects presets per output channel, sends channel MIDI, and silences notes.

Do not restore FluidSynth's MIDI driver. Doing so would create two competing
input paths and make MMC/SysEx handling and ALSA port ownership unclear. Do
not attempt to replace FluidSynth with libremidi: libremidi does not synthesize
SoundFonts or provide audio output.

Input callbacks may originate outside the application thread. Exceptions must
not escape a MIDI callback. The startup gate remains closed until inputs have
been connected and an all-notes-off pass has run; this guards against startup
device noise. Ignored active-sensing/timing traffic and hotplug behavior are
part of the input adapter's contract.

## Configuration contract

The configuration schema is deliberately strict and versioned. The current
required version is 6.

- The version in the file must exactly match the compiled
  `required_schema_version` constant. There is no backward-compatibility
  requirement unless explicitly agreed for a future schema.
- Reject missing fields, unknown fields, empty required lists, duplicate
  SoundFont IDs, invalid ranges, unsupported control commands, and overlapping
  action bindings. Do not silently ignore a typo in stage configuration.
- Paths relative to the YAML file are resolved relative to that file, not the
  process working directory.
- The `soundfonts` list is ordered and non-empty. Files are loaded eagerly, and
  repeated references to one file reuse its loaded FluidSynth ID.
- Each `soundfonts` entry has an optional controller note-name `key`. Names use
  the SE49-documented octave convention, where MIDI 60 is `C3`, across the
  one-digit MIDI domain `C0` through `G8` (MIDI 24 through 127). Keys are
  interpreted before octave transposition and independently of MIDI channel.
  They must be unique and must not reuse the physical key of any configured
  Note action. At least one entry has a key exactly when
  `controls.soundfont_by_note` is configured.
- `midi_control_change_mappings` is optional; omission means that no Control
  Change normalization is needed. Each entry matches one exact source-port
  display name, human-facing channel, and controller number, then replaces only
  the controller number. Duplicate source/channel/controller matches are
  rejected.
- `controls.recording`, `controls.octave_down`, and `controls.octave_up` each
  contain exactly one required binding. `controls.next_soundfont` and
  `controls.soundfont_by_note` are individually optional, but at least one is
  required. Performance setup changes are made by editing bindings before
  startup. Supported binding types are Note On, exact Control Change, exact or
  any Program Change, and named MMC commands.
- A schema shape change requires incrementing the exact version constant,
  updating `zeta.example.yaml` and README usage, and adding positive and
  negative parser tests.

## Concurrency and lifecycle

- `Application` owns dependencies in destruction-safe order. MIDI input is
  stopped before shutdown tears down the FSM output implementation.
- `LooperFsm` protects its current state and shared state data with a mutex.
- Playback uses `std::jthread`, a stop token, a condition variable, and a
  generation counter so a take can be interrupted without polling or long
  sleeps.
- Shutdown is idempotent. It stops loop playback, releases sustained/active
  notes, stops and joins the worker, and wakes `Application::run()`.
- Do not put blocking I/O, SoundFont loading, or unbounded work on the MIDI
  callback or playback scheduling paths.
- The take has a fixed maximum event count. Any change to real-time allocation
  behavior or capacity policy needs explicit performance reasoning and tests.

## Coding rules

- Use C++20, RAII, clear ownership, narrow interfaces, and project-owned types
  at architectural boundaries.
- Keep a function used by only one class as a private static member when it
  does not require instance state. Do not expose class implementation details
  as namespace-level helpers merely to make them directly testable.
- Keep changes DRY, small, and local to the owning layer. Prefer an explicit
  helper over repeated transition or routing logic, but do not invent an
  abstraction before it has a real responsibility.
- Favor readable control flow. Do not use C++ initializer-statements in
  conditions. Write the operation and the condition separately:

  ```cpp
  const auto error = input->open_port(port, client_name);
  if (error.is_set()) {
      // ...
  }
  ```

- Do not introduce `std::variant` into the FSM design.
- Follow the existing naming and formatting, compile with `-Wall -Wextra
  -Wpedantic`, and keep warnings clean.
- Do not use unexplained numeric literals for domain limits, encodings, or
  conversions. Introduce named constants and comments for non-obvious numeric
  conventions so their meaning and source remain visible at the use site.
- Preserve user changes in a dirty worktree and avoid unrelated formatting or
  cleanup.
- Update tests and documentation in the same change as behavior or schema.
- Update `CHANGELOG.md` in every change, placing pending entries under
  `Unreleased`.
- Push back on complexity that does not serve a demonstrated live-performance
  requirement. Architectural layering changes require prior agreement.

## Dependency policy

The current dependency strategy is intentional:

- System packages provide platform-facing ALSA and FluidSynth libraries.
- CMake first looks for libremidi 5.4.3 and yaml-cpp, then uses pinned
  `FetchContent` fallbacks.
- GoogleTest and Hegel are pinned and fetched only for test builds. Hegel is a
  selective beta pilot for pure property tests; retain deterministic GoogleTest
  examples and do not treat it as a blanket testing standard. Follow
  [HEGEL.md](HEGEL.md) when selecting, writing, and running properties.

Do not add Conan, vcpkg, or Nix merely for uniformity. At the current project
size they would add another packaging layer without removing the need for
Raspberry Pi system audio integration. Reconsider a package manager if the
dependency graph grows materially, repeatable cross-compilation becomes a
real requirement, or the current pinned/system split causes reproducibility
failures. Such a change is a dedicated build-architecture decision.

## Building and testing changes

Configure a development build and run the complete suite:

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI also runs clang-tidy 18 over first-party production sources. To reproduce
that check locally:

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
run-clang-tidy-18 \
    -p build \
    -j 2 \
    "$(pwd)/[^/]+\.cpp$"
```

GCC's analyzer is limited to the production target so fetched dependencies are
not analyzed. Run it in a separate build directory:

```bash
cmake -S . -B build-analyzer \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=OFF \
    -DZETA_GCC_ANALYZER=ON
cmake --build build-analyzer --parallel
```

For routing diagnostics, configure a separate trace build or reconfigure the
existing build with `-DZETA_MIDI_TRACE=ON`. Do not leave unconditional MIDI
logging on a performance path.

The test suites divide responsibilities as follows:

- `configuration_tests`: strict schema, ranges, path resolution, and binding
  ambiguity.
- `midi_tests`: library-independent byte decoding and MMC recognition.
- `midi_control_change_mapping_tests`: exact per-port Control Change mapping,
  passthrough, value preservation, and one-pass behavior.
- `octave_transposer_tests`: octave bounds, arithmetic key transposition, and
  non-key MIDI passthrough.
- `soundfont_selector_tests`: sequential/direct current-selection consistency
  and bounded physical-key lookup against the ordered catalog.
- `looper_fsm_tests`: state transitions and output-alphabet calls using mocks.
- `current_behavior_tests`: application-level behavior with fake MIDI and
  FluidSynth boundaries.

Every FSM stimulus should be tested in every state where its behavior differs.
Test both the requested output action and the returned/installed `StateId`.
Configuration changes need rejection tests, not only a happy path. Run
`git diff --check` in addition to the complete suite.

## Before submitting

- Confirm that all playable notes and unbound expressive controls still reach
  the synthesizer.
- Confirm that the first played note still defines recording time zero.
- Confirm that live and loop channel behavior remain independent.
- Confirm octave changes preserve the recorded loop's pitch after recording
  starts.
- Confirm clean Ctrl-C/SIGTERM shutdown and no stuck notes.
- Confirm the change is usable without a screen or computer keyboard.
- Confirm the complete test suite passes in both normal and, when routing is
  affected, trace builds.
- Describe any deliberate user-visible limitation candidly.
