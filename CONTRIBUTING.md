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
LooperFsm -> current master State -> LooperOutput
                                         |
                                         v
                                     Application
                                     /         \
                            SynthEngine      LoopSlotGroup
                            (FluidSynth)       /       \
                                      GuideLoopSlot  RegularLoopSlot ...
                                          dedicated worker per slot
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
  the master FSM output alphabet, and owns the live MIDI route. It delegates
  every slot command to the group and never accesses an owned slot directly.
- `loop_slot_group.*` is the aggregate boundary. It owns the ordered slots,
  bounded pending take, raw-key lookup, guide-first invariant, and cross-slot
  stop behavior. It creates the first slot as the guide and later slots as
  regular slots, then coordinates them only through the common base interface.
- `loop_slot_fsm.*` owns the dependency-free subordinate playback state
  machine and its start, mute, and termination command semantics.
- `loop_slot.*` defines the common slot mechanism and its final guide and
  regular role implementations. Every slot encapsulates its identity, key,
  FluidSynth channel, locked SoundFont/octave state, immutable committed take,
  subordinate playback FSM, synchronization, and eagerly created worker. Role
  commands perform their own behavior; callers do not query role predicates.
- `loop_timing.*` constructs immutable guide and regular playback schedules as
  pure domain arithmetic, independent of workers, MIDI, and FluidSynth.
- `pending_take.*` owns bounded in-progress events and accepted held-note state,
  including release synthesis and musical-content duration at completion.
- `octave_transposer.*` owns octave bounds and arithmetic transposition for one
  MIDI route. `Application` owns the live instance and each `LoopSlot` owns its
  independent recorded route.
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
  `loopSlotControlPressed`, `nextSoundFontPressed`,
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

The master states are `Ready`, `ReadySelectingLoopSlot`,
`ReadySelectingSoundFont`, `Armed`, `ArmedSelectingSoundFont`, `Recording`, and
`Stopped`.

- `Ready`: MIDI is monitored on live channel 0. The loop-slot control enters
  `ReadySelectingLoopSlot`; Next, direct SoundFont selection, and octave
  controls affect the live route.
- `ReadySelectingLoopSlot`: the next positive-velocity Note On is matched by
  raw key and consumed regardless of incoming channel. Selecting the Muted
  guide clears its old take, snapshots the live SoundFont/octave into it,
  installs its `SlotId` as the sole recording slot, and enters `Armed`. A Muted
  regular slot does the same only while the guide is Looping; otherwise the
  selection is rejected and the master returns to `Ready`. Selecting a Looping
  regular slot stops only itself. Selecting the Looping guide stops and
  discards every regular slot before stopping itself. An unmapped note is
  consumed and returns to `Ready`; pressing the selector again cancels.
- `Armed`: MIDI is monitored on the selected slot's channel. The first
  positive-velocity Note On enters `Recording` and is stored at offset zero.
  SoundFont and octave changes may update this pending route. The loop-slot
  control alone cancels and clears it; slot selection is unavailable.
- `Recording`: Note On and Note Off events are timestamped into the one pending
  take. SoundFont and octave controls are inert. The loop-slot control alone
  finalizes the bounded take, tells that slot to construct and activate its
  schedule, clears the master's selected `SlotId`, and returns to `Ready`.
  Completing a regular take leaves the guide and its peers running.
- `Stopped`: all performer stimuli are inert. Process shutdown terminates every
  slot worker and silences every configured channel before entering it.

`ReadySelectingSoundFont` and `ArmedSelectingSoundFont` preserve the existing
one-shot raw-note SoundFont workflow on the live and selected-slot routes.
Selection notes are consumed; an unmapped note changes nothing; pressing the
selector again cancels. Other MIDI retains the selector state, while another
configured action performs its ordinary behavior and exits it. Recording
ignores the SoundFont selector.

Each `LoopSlot` owns a subordinate `Muted`, `Looping`, or `Terminated` playback
FSM. `startRequested` activates only a Muted slot after a newly completed take;
`muteRequested` deactivates a Looping slot and discards its take; and
`terminationRequested` terminates any nonterminal slot exactly once. The slot
role determines whether its selection command also requests the group-wide
dependent stop. Muted means only not currently playing and never means
resumable.

Live output uses FluidSynth channel 0; slot catalog index N uses channel N+1.
Recorded notes retain velocity and store the key produced by that slot's
transposer. Once recording starts, the slot's SoundFont and octave are locked.
Completing or canceling adopts the current selection for live output, while
already looping slots retain their own locked selection.

The first configured slot is the guide. It must be looping before a regular
slot can arm. Guide recording start `R` and completion `C` define period
`T = C - R`; its first playback cycle starts at `C`. A regular recording start
captures its phase modulo `T`, and the phrase may span any number of guide
cycles. Its period is the smallest positive whole multiple of `T` covering its
musical content, and its first playback cycle is the first matching captured
phase at or after completion. Workers derive every repetition from these
immutable absolute deadlines rather than their actual wake time, so late
dispatch cannot accumulate drift.

The final matching Note Off defines musical content end; trailing delay before
the completion control is discarded. Any accepted note still held at
completion receives a matching Note Off at completion, and the slot channel is
silenced before playback. This synchronization is relative to the guide only;
notes are not beat-quantized. Stopping a regular slot affects only itself.
Stopping the guide stops and discards all regular takes first, and a newly
recorded guide establishes a new timeline.

Octave transposition starts at zero, moves in twelve-semitone steps, and clamps
from three octaves down through four octaves up. The performer changes octaves
only while no notes are playing. Do not add held-note tracking or behavior for
octave changes during active notes without changing this contract explicitly.
Arming snapshots the current live transposition into the selected slot.
Changes while Armed update both the live and pending route; later live changes
do not affect any already looping slot.
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
required version is 7.

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
- The `loop_slots` list is ordered and non-empty. Every scalar entry is one raw
  physical controller note name; catalog order defines its stable `SlotId`, the
  first entry is implicitly the guide, and every later entry is regular. Keys
  must be unique and must not reuse any configured Note action.
  They may overlap SoundFont-selection keys because the selector controls
  disambiguate those one-shot gestures.
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
- `controls.loop_slot_by_note`, `controls.octave_down`, and
  `controls.octave_up` each contain exactly one required binding.
  `controls.next_soundfont` and
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
- Every configured slot eagerly owns one sleeping `std::jthread`. Each worker
  uses a stop token, condition variable, `wait_until`, and generation counter
  so a take can be interrupted without polling, whole-cycle sleeps, or waiting
  for another loop boundary. Repetition deadlines advance from the immutable
  absolute schedule, never from the worker's wake time.
- `LoopSlotGroup` records into the sole `PendingTake`. Completion gives the
  selected worker one immutable snapshot; recording and playback never share a
  mutable event vector.
- `PendingTake` preserves the fixed event bound while reserving one closure
  event for every accepted held note. It performs no recording-path allocation.
- A slot does not hold its command mutex while calling the narrow group output;
  the guide stop command may synchronously deactivate dependent slots. The
  group stops dependents before the guide and outlives every owned slot.
- Slot deactivation invalidates its generation while ordering worker dispatch
  and per-channel silencing under the same mutex. When stop returns, no event
  from that take can be emitted, and immediate re-arming is safe.
- Shutdown is idempotent. It terminates and joins every slot worker, releases
  sustained/active notes on every channel, and wakes `Application::run()`.
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
- `loop_slot_fsm_tests`: subordinate playback-FSM transitions and its Hegel
  command-sequence model property.
- `loop_timing_tests`: deterministic timing boundaries plus Hegel properties
  for the smallest covering guide multiple, first matching phase, and phase
  preservation.
- `pending_take_tests`: bounded event/held-note accounting, synthesized release,
  trailing-silence trimming, and reset behavior.
- `current_behavior_tests`: guide-first gating, guide cascade, regular-slot
  isolation and replacement, synchronized playback, raw-note consumption,
  clean held-note completion, and shutdown with fake MIDI and FluidSynth
  boundaries.

Every FSM stimulus should be tested in every state where its behavior differs.
Test both the requested output action and the returned/installed `StateId`.
Configuration changes need rejection tests, not only a happy path. Run
`git diff --check` in addition to the complete suite.

## Before submitting

- Confirm that all playable notes and unbound expressive controls still reach
  the synthesizer.
- Confirm that the first played note still defines recording time zero.
- Confirm that the guide must start first, regular phases remain synchronized,
  and arbitrary-length regular phrases repeat on whole guide cycles.
- Confirm that stopping a regular slot leaves peers running and stopping the
  guide stops every regular slot without stuck notes.
- Confirm that live and every loop-slot channel remain isolated.
- Confirm octave changes preserve the recorded loop's pitch after recording
  starts.
- Confirm clean Ctrl-C/SIGTERM shutdown and no stuck notes.
- Confirm the change is usable without a screen or computer keyboard.
- Confirm the complete test suite passes in both normal and, when routing is
  affected, trace builds.
- Describe any deliberate user-visible limitation candidly.
