# Changelog

All notable changes to Zeta DAW are documented in this file.

## [Unreleased]

### Added

- Added direct SoundFont selection by pressing a configured controller action
  and then a keyed piano note, with explicit Ready, Armed, and Looping
  selection states for live performance.
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

- Configuration schema 6 makes sequential Next and direct note selection
  independently optional while requiring at least one SoundFont-selection
  mechanism.
- Direct-selection keys are now optional SE49 physical-key names on their
  `soundfonts` entries and identify raw physical keys independently of MIDI
  channel; the separate arbitrary channel/key mapping list was removed.
- SoundFont catalog navigation and bounded physical-key lookup are encapsulated
  in a dependency-free selector shared by sequential and direct selection.
- Clarified that performer-visible interaction phases must be explicit looper
  states and must not be encoded as hidden modes in shared state data or other
  layers.
- Clarified Linux-only platform support and the currently CI-tested compiler
  without making GCC mandatory for normal builds.
- Renamed the production CMake target and application executable from
  `midi_looper` to `zd`.
- Renamed the FSM recording-control stimulus for consistency with the
  configured action, without changing state behavior.
- The recording control now cancels an armed take or stops an active loop and
  returns to Ready; only process shutdown stops the playback worker and exits
  the application, and returning to Ready preserves independent live and loop
  octave selections for later takes.
- Recording completion now always follows one FSM transition into looping;
  zero-length playback requests are rejected by the playback worker so they
  cannot busy-loop.
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
