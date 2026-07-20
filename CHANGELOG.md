# Changelog

All notable changes to Zeta DAW are documented in this file.

## [Unreleased]

### Added

- Added the `zfont` CLI helper to list and automatically audition every preset
  in an SF2 or SF3 file through Zeta's configured FluidSynth audio output.
- Documented a measured, reversible Raspberry Pi headless operating profile
  with local on-demand graphical maintenance and disabled unused Bluetooth,
  and added a PlantUML diagram of the complete runtime message sequence.
- Added optional FluidSynth audio driver, ALSA device, and gain configuration,
  including Raspberry Pi USB-audio, real-time priority, locked-memory, and
  systemd setup guidance while preserving existing desktop defaults.
- Added guide synchronization: the first configured slot establishes the
  timeline, while every later slot preserves its recorded guide-relative phase
  and repeats at the smallest whole guide multiple covering its phrase.
- Added a `LoopSlotGroup` aggregate, sibling guide/regular slot roles, pure
  playback-schedule arithmetic, and bounded pending-take ownership so
  cross-slot policy no longer accumulates in `Application`.
- Added deterministic and Hegel property coverage for synchronized timing, plus
  pending held-note, guide-first, cascade, regular-isolation, and late-worker
  absolute-deadline regressions.
- Added ordered loop slots that can be recorded, stopped, replaced, and played
  concurrently from one-shot raw-note selection gestures, with one dedicated
  FluidSynth channel and eagerly created worker per configured slot.
- Added a subordinate Muted/Looping/Terminated playback FSM and an independent
  Hegel command-sequence model property alongside deterministic master-FSM and
  application concurrency regressions.
- Added direct SoundFont selection by pressing a configured controller action
  and then a keyed piano note, with explicit Ready and Armed selection states
  for live and pending-slot routes.
- Added a selective Hegel property-testing pilot and contributor guide for
  octave transposition, Control Change mapping, and MIDI control-binding
  overlap contracts while retaining the deterministic GoogleTest examples.
- Added exact, source-port-aware MIDI Control Change mappings so controller
  faders can use standard synthesizer controller semantics without persistent
  hardware reconfiguration.
- Added GitHub Actions validation for the complete build and test suite,
  ShellCheck, clang-tidy, and GCC static analysis.
- Added `build_debug.sh` for a separate debug build with MIDI routing traces.
- Restored the SE49 Octave Down and Octave Up controls while MMC transport mode
  is active, with arithmetic transposition from three octaves down through four
  octaves up.

### Changed

- Clarified unattended Raspberry Pi systemd installation and reboot
  verification, persistent recovery from late USB-audio startup, service
  permissions, diagnostics, and the boundary between required settings and
  optional real-time hardening.
- Configuration schema 8 adds the optional strict `audio` mapping.
- Upgraded the test-only Hegel pin to v0.7.4 and migrated the subordinate
  playback lifecycle property to native named-rule stateful testing while
  retaining deterministic worker and transition coverage.
- Newly completed regular slots now join their current natural repetition,
  omitting only its already elapsed event prefix once while keeping every later
  repetition complete and on the original absolute timeline.
- Configuration schema 7 replaces `controls.recording` with
  `controls.loop_slot_by_note` and requires a non-empty ordered `loop_slots`
  catalog of unique raw physical-note names.
- The master GoF FSM now owns the sole armed/recording slot while subordinate
  slot FSMs own only playback; completing or canceling a take needs only the
  loop-slot control. Stopping or replacing a regular slot leaves peers running;
  stopping the guide stops and discards every regular slot first.
- Loop-slot commands now delegate through `LoopSlotGroup` and polymorphic slot
  behavior, leaving `Application` responsible for orchestration and its live
  route rather than slot catalog, pending-take, or role decisions.
- Recording completion trims silence after the last released note and emits
  matching releases for accepted notes still held at completion.
- The pure subordinate playback FSM and its Hegel model test now build without
  FluidSynth or worker dependencies.
- FluidSynth's MIDI-channel count is configured before synth creation so live
  output remains on channel 0 and every loop slot has an isolated channel.
- FluidSynth setting return codes now use `FLUID_OK`, fixing startup failures
  after successfully configuring the MIDI-channel count.
- Sequential Next and direct note selection are independently optional while
  at least one SoundFont-selection mechanism remains required.
- Direct-selection keys are now optional controller physical-key names on
  their `soundfonts` entries, using the SE49-inspired octave convention across
  the generic one-digit MIDI domain and independently of MIDI channel; the
  separate arbitrary channel/key mapping list was removed.
- Documented that direct selection uses the controller-emitted raw MIDI key
  and that stored SE49 octave and transpose offsets must be cleared before
  enabling MMC transport mode.
- SoundFont catalog navigation and bounded physical-key lookup are encapsulated
  in a dependency-free selector shared by sequential and direct selection.
- Clarified that performer-visible interaction phases must be explicit looper
  states and must not be encoded as hidden modes in shared state data or other
  layers.
- Clarified Linux-only platform support and the currently CI-tested compiler
  without making GCC mandatory for normal builds.
- Renamed the production CMake target and application executable from
  `midi_looper` to `zd`.
- The loop-slot control cancels an armed take or completes an active recording
  without requiring the slot note again; only process shutdown terminates slot
  workers and exits the application.
- Recording completion now follows one master transition back to `Ready` while
  requesting playback from the selected slot; zero-length playback requests
  are rejected by the slot worker so they cannot busy-loop.
- Configuration schema 5 accepts optional `midi_control_change_mappings` for
  controllers needing normalization, and requires dedicated `octave_down` and
  `octave_up` bindings with overlap rejection among all four application
  actions.
- Live and pending-loop octave selection start synchronized for the first take;
  recorded loops retain their selected octave while later changes affect only
  live playing.
- Octave transposition leaves a key unchanged when shifting it would exceed the
  MIDI range instead of requiring special handling from callers.
- Schema-version errors report both the provided and required versions.
- Contributor guidance now requires agents to propose material adjacent
  requirements and policy decisions while keeping their implementation gated
  on project-owner approval.
- Contributor guidance now requires remote-base and complete PR-range audits so
  unpublished work is not silently mixed into a ticket branch.
- Release builds produced by `build.sh` now explicitly disable MIDI tracing
  instead of inheriting a cached CMake option.

### Fixed

- Eliminated cumulative phase drift between independently recorded slots by
  deriving every regular playback deadline from the guide's immutable absolute
  schedule instead of completion time or the previous worker wake-up.
- Rejected channel voice messages containing out-of-domain MIDI data bytes at
  the raw decoder boundary, preventing malformed Control Change input from
  indexing beyond the mapping table.
- Restored the SE49 volume fader while MMC transport mode is active by mapping
  its configured MIDI2 channel-16 CC20 event to Channel Volume CC7.
- Restored MIDI input after hardware ports disconnect and reconnect by keeping
  libremidi's observed-port lifecycle synchronized with active connections.

## [0.1.0] - 2026-07-14

### Added

- A C++20 MIDI looper with a GoF-style finite state machine for ready, armed,
  recording, looping, and stopped behavior.
- Live monitoring and timestamped note-loop playback on independent
  FluidSynth channels.
- First-note recording start, so a take begins without leading silence.
- Strict, versioned YAML configuration for an ordered list of eagerly loaded
  `.sf2`/`.sf3` SoundFonts and MIDI controller actions.
- Runtime SoundFont selection: choose the pending loop sound before recording
  and independently cycle the live sound while the loop plays.
- Note, Control Change, exact/any Program Change, and MIDI Machine Control
  bindings, with ambiguous action bindings rejected at startup.
- Nektar SE49 MMC setup using Transpose Down for recording and Transpose Up for
  SoundFont selection without reserving a piano key.
- Headless Raspberry Pi operation, automatic MIDI device hotplug/reconnection,
  and desktop execution with graceful SIGINT/SIGTERM shutdown.
- Unit and integration coverage for configuration, MIDI decoding, FSM
  transitions, and current application behavior.

### Changed

- Split MIDI input from synthesis: libremidi now owns ALSA Sequencer input and
  SysEx/MMC reception, while FluidSynth is used only for SoundFont synthesis
  and audio.
- Renamed the entry point and MIDI source files to make their responsibilities
  explicit (`main.cpp` and `midi_event.*`).
- Constructor-injected the invariant `LooperOutput` and shared `LooperStateData`
  dependencies into every FSM state, keeping stimulus signatures focused on
  event data.
- Simplified each configured controller action to one required binding.

### Fixed

- Silenced and gated MIDI during startup to prevent device connection traffic
  from producing an unintended note.
- Restored the current live SoundFont when a recorded take transitions into
  loop playback.
- Ensured active notes and sustain are released during shutdown.

### Known limitations

- Pressing the recording control while already looping stops the loop and exits
  instead of returning to the ready state.
- On the SE49, MMC transport mode currently repurposes both Octave buttons, so
  octave shifting is unavailable until application-level MMC octave handling
  is added.
