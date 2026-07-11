# Zeta DAW MIDI Looper

Zeta DAW is a small C++20 MIDI looper using libremidi for controller input and
FluidSynth for SoundFont synthesis. It is intended
to run either as a normal Linux desktop process or as a headless service on a
Raspberry Pi 5. Once started, performance control comes entirely from a MIDI
controller.

The current workflow records one take and repeats it while routing subsequent
playing through a separate live SoundFont:

1. Press a configured MIDI recording control to arm the looper.
2. Play the first note. Recording begins at that note, avoiding leading
   silence.
3. Press a configured recording control again to finish the take and start
   looping.
4. Continue playing live over the loop.

Currently, pressing the recording control a third time stops playback and
exits the application. Returning to the initial state instead is planned as a
separate behavioral change.

## Requirements

- Linux, including Raspberry Pi OS or Ubuntu
- A C++20 compiler
- CMake 3.22 or newer
- pkg-config
- FluidSynth development files
- ALSA development files
- yaml-cpp development files, recommended
- An ALSA-compatible audio output and MIDI controller
- One or more SoundFont files selected before the performance

On Raspberry Pi OS, Debian, or Ubuntu, install the build dependencies with:

```bash
sudo apt update
sudo apt install \
    build-essential \
    cmake \
    pkg-config \
    libasound2-dev \
    libfluidsynth-dev \
    libyaml-cpp-dev
```

The repository has pinned CMake fallbacks for libremidi and yaml-cpp when
matching system packages are unavailable. These fallbacks require network
access during the first CMake configuration. libremidi receives MIDI and MMC
SysEx through ALSA Sequencer; FluidSynth is used only for synthesis and audio.

These optional packages are useful when setting up a machine:

```bash
sudo apt install alsa-utils fluid-soundfont-gm
```

`alsa-utils` supplies tools such as `aconnect` and `aseqdump`. The
`fluid-soundfont-gm` package supplies a General MIDI SoundFont for initial
testing; use the SoundFonts chosen for the actual performance afterward.

## Build

For a release build without the test suite:

```bash
./build.sh
```

The executable is written to `build/midi_looper`.

To build and run all tests:

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

GoogleTest is downloaded through CMake FetchContent the first time tests are
configured, so that configuration requires network access.

To include detailed MIDI routing logs in a diagnostic build:

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON \
    -DZETA_MIDI_TRACE=ON
cmake --build build --parallel
```

## Configuration file

Copy the example and edit it for the available SoundFonts and controller:

```bash
cp zeta.example.yaml zeta.yaml
```

Run with an explicit configuration path during development:

```bash
./build/midi_looper ./zeta.yaml
```

When no argument is given, the application reads:

```text
/etc/zeta-daw/zeta.yaml
```

Install a finished configuration there with:

```bash
sudo install -d /etc/zeta-daw
sudo install -m 0644 zeta.yaml /etc/zeta-daw/zeta.yaml
```

Relative SoundFont paths are resolved relative to the configuration file, not
the process working directory. Absolute paths are recommended for a system
service.

### Complete example

```yaml
schema_version: 1

soundfonts:
  - id: piano
    file: /srv/zeta-daw/soundfonts/grand-piano.sf2
    bank: 0
    preset: 0

  - id: bass
    file: /srv/zeta-daw/soundfonts/electric-bass.sf2
    bank: 0
    preset: 34

parts:
  live: piano
  loop: bass

controls:
  recording:
    - type: program_change
      channel: 1
      program: any
```

The parser rejects missing fields, unknown fields, duplicate SoundFont IDs,
invalid references, and out-of-range MIDI values. Configuration errors stop
startup before the audio or MIDI drivers are created.

### SoundFonts and parts

`soundfonts` is an ordered list. Every entry has:

- `id`: a unique name used elsewhere in the configuration
- `file`: an absolute path or a path relative to the YAML file
- `bank`: the SoundFont bank, from 0 through 16383
- `preset`: the preset, from 0 through 127

Every configured SoundFont is loaded eagerly before MIDI input starts. This
avoids disk-loading delays during a performance. If multiple entries reference
the same file, the file is loaded once and can still be used with different
banks or presets.

`parts.live` selects the SoundFont used for playing over a completed loop.
`parts.loop` selects the SoundFont used while recording and for loop playback.
The configured live program is restored when loop playback begins.

Only list SoundFonts required for the performance, because all listed files
remain resident in memory.

### Recording controls

All entries under `controls.recording` behave like the original Enter-key
control. A matched event is consumed: it is not sent to FluidSynth and is not
recorded in the loop.

YAML MIDI channels use the human-facing range 1 through 16.

#### MIDI note

```yaml
controls:
  recording:
    - type: note
      channel: 1
      key: 84
```

The binding matches a positive-velocity Note On. The selected key becomes a
dedicated control and does not sound. Octave or transpose changes on the
controller can change the MIDI note number.

#### Control Change

```yaml
controls:
  recording:
    - type: control_change
      channel: 1
      controller: 64
      value: 127
```

This is useful for a momentary footswitch. When CC64 is consumed as a recording
control, that pedal cannot simultaneously provide sustain.

#### Exact Program Change

```yaml
controls:
  recording:
    - type: program_change
      channel: 1
      program: 12
```

Only Program Change 12 on channel 1 matches.

#### Any Program Change

```yaml
controls:
  recording:
    - type: program_change
      channel: 1
      program: any
```

Every Program Change on channel 1 matches and is consumed. This is useful for
increment/decrement buttons such as the repurposed Transpose buttons on a
Nektar SE49. Do not use this form if other Program Changes on the same channel
must reach the synthesizer or control another feature.

Multiple recording bindings may be listed. Any matching binding activates the
same state-dependent recording control.

## Nektar SE49 example

The SE49 Transpose buttons normally transpose the keyboard. They can instead
be assigned once to send Program Change messages. The assignment is stored by
the controller across power cycles.

Perform this one-time controller setup:

1. Press **Octave Up + Transpose Up** simultaneously. The Setup LED should
   blink orange.
2. Press the keyboard key labeled **D2 / MIDI Program Change** in the
   Transpose-button assignment section.
3. Press the keyboard key labeled **Enter (C5)** to save and leave Setup.

Do not enter a program number. This procedure assigns the function of the two
Transpose buttons; it does not send a particular Program Change.

Use this Zeta configuration:

```yaml
controls:
  recording:
    - type: program_change
      channel: 1
      program: any
```

During a performance:

1. Press **Transpose Up** to arm recording.
2. Play the first note to begin recording.
3. Press **Transpose Down** to finish the take and start the loop.

Alternating Up and Down prevents the controller's program counter from
eventually reaching its limit. Both buttons send Program Change messages and
are equivalent from Zeta's perspective; the current FSM state determines what
the press does.

If the buttons do not activate Zeta, check that the controller's MIDI channel
matches the `channel` value in the YAML. To inspect the actual messages, stop
Zeta and use ALSA's diagnostic tools:

```bash
aconnect -l
aseqdump -l
aseqdump -p CLIENT:PORT
```

Replace `CLIENT:PORT` with the SE49 input shown by `aseqdump -l`, then press
the Transpose buttons. The output should contain Program Change events.

The official setup procedure and control assignments are documented in the
[Nektar SE49/SE61 Owner's Manual](https://support.nektartech.com/wp-content/uploads/2023/05/SE49_61_printed_guide_v1_3_ENGLISH.pdf).

## Running on the desktop

Connect the audio interface and MIDI controller before starting Zeta, then
run:

```bash
./build/midi_looper /path/to/zeta.yaml
```

Press Ctrl-C to shut down gracefully. SIGTERM is handled the same way.

## Starting automatically on Raspberry Pi

Install the executable and configuration:

```bash
sudo install -m 0755 build/midi_looper /usr/local/bin/midi_looper
sudo install -d /etc/zeta-daw
sudo install -m 0644 zeta.yaml /etc/zeta-daw/zeta.yaml
```

Make sure the service user can access ALSA devices. On Raspberry Pi OS this
normally means membership in the `audio` group:

```bash
sudo usermod -aG audio YOUR_USER
```

Create `/etc/systemd/system/zeta-daw.service`:

```ini
[Unit]
Description=Zeta DAW MIDI looper
After=sound.target

[Service]
Type=simple
User=YOUR_USER
SupplementaryGroups=audio
ExecStart=/usr/local/bin/midi_looper /etc/zeta-daw/zeta.yaml
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
```

Replace `YOUR_USER` with the account that owns the performance setup. Ensure
that account can read every configured SoundFont path.

Enable and start the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now zeta-daw.service
```

The MIDI controller may be connected before or after Zeta starts. Hardware
MIDI inputs are connected automatically, and libremidi observes disconnects
and reconnects without restarting the service.

Inspect status and logs with:

```bash
systemctl status zeta-daw.service
journalctl -u zeta-daw.service -f
```

Stop or restart it with:

```bash
sudo systemctl stop zeta-daw.service
sudo systemctl restart zeta-daw.service
```

`systemd` sends SIGTERM when stopping the service, allowing Zeta to silence
active notes and release its MIDI and audio resources cleanly.

## Troubleshooting

### Configuration fails at startup

Read the reported YAML location and field name. The schema is intentionally
strict; misspelled or unsupported fields are errors rather than ignored
settings.

### A SoundFont cannot be loaded

Verify that the path exists and is readable by the desktop or service user.
For a system service, prefer absolute paths and avoid files inside a user's
private home directory.

### The controller does not trigger recording

- Confirm that the controller is connected before Zeta starts.
- Check its events with `aseqdump` while Zeta is stopped.
- Verify the MIDI channel and event values against the YAML.
- For the SE49, confirm that its Transpose buttons emit Program Change rather
  than changing the keyboard transpose setting.

### Notes use the wrong SoundFont after recording

Confirm that `parts.live` and `parts.loop` reference the intended IDs. Zeta
restores the configured live SoundFont, bank, and preset when loop playback
begins. A controller event intended for another purpose should be configured
as a consumed recording control if it must not reach FluidSynth.
