# Changelog

All notable changes to Zeta DAW are documented in this file.

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
- Constructor-injected the invariant `LooperOutput` dependency into every FSM
  state, keeping stimulus signatures focused on event data.

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
