# Zeta DAW MIDI Looper

Zeta DAW is a C++20 MIDI looper for Linux, including Raspberry Pi 5. It can
run as a desktop process or start automatically as a headless service and be
controlled entirely from a MIDI controller.

The performance workflow is:

1. Use the configured Next control to select a SoundFont.
2. Press the configured recording control to arm the looper.
3. Play the first note. Recording starts on that note, with no leading
   silence.
4. Press the recording control again to finish the take and start looping.
5. Keep playing live and select other SoundFonts without changing the loop.

Pressing the recording control a third time currently stops playback and exits
the application.

## Requirements and installation

- Linux, including Raspberry Pi OS or Ubuntu
- A C++20 compiler
- CMake 3.22 or newer
- pkg-config
- FluidSynth and ALSA development files
- An ALSA-compatible audio output and MIDI controller
- One or more `.sf2` or `.sf3` SoundFont files

On Raspberry Pi OS, Debian, or Ubuntu:

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

CMake downloads pinned copies of libremidi and, when it is not installed,
yaml-cpp during the first configuration. That step requires network access.

These optional packages provide MIDI diagnostic tools and a General MIDI
SoundFont suitable for an initial test:

```bash
sudo apt install alsa-utils fluid-soundfont-gm
```

## Build

Configure and build a release executable:

```bash
./build.sh
```

The release executable is `build/midi_looper`. This script explicitly disables
MIDI tracing, even when the build directory previously cached another value.

For a debug executable with MIDI routing traces enabled, use the separate debug
build directory:

```bash
./build_debug.sh
```

The trace executable is `build-debug/midi_looper`.

## Configuration

Copy the example and edit it for the available SoundFonts and controller:

```bash
cp zeta.example.yaml zeta.yaml
```

A complete configuration looks like this:

```yaml
schema_version: 5

midi_control_change_mappings:
  - source_port: "SE49 MIDI2"
    channel: 16
    controller: 20
    target_controller: 7

soundfonts:
  - id: piano
    file: /srv/zeta-daw/soundfonts/grand-piano.sf2
    bank: 0
    preset: 0

  - id: bass
    file: /srv/zeta-daw/soundfonts/electric-bass.sf2
    bank: 0
    preset: 34

controls:
  recording:
    type: machine_control
    command: rewind

  next_soundfont:
    type: machine_control
    command: stop

  octave_down:
    type: machine_control
    command: play

  octave_up:
    type: machine_control
    command: record_strobe
```

Only schema version 5 is accepted. A configuration error stops startup and
reports the invalid field.

### SoundFonts

`soundfonts` is an ordered, non-empty list. Every entry has:

- `id`: a unique name shown in the selection log
- `file`: an absolute path or a path relative to the YAML file
- `bank`: the SoundFont bank, from 0 through 16383
- `preset`: the preset, from 0 through 127

Only include the sounds needed for the performance. They are prepared during
startup, and the first entry is selected initially. Next advances through the
list and wraps to the first entry.

The SoundFont selected when recording is armed is used for the loop. Next may
change that selection while armed but before the first note. During recording,
Next is ignored. During loop playback, Next changes the live sound without
changing the recorded loop.

To inspect the banks and presets in a SoundFont, start FluidSynth with the
file, then use its `fonts` and `inst` shell commands. Consult your distribution's
FluidSynth documentation because command-line audio options differ by system.

### MIDI Control Change mappings

`midi_control_change_mappings` is a required list and may be empty. Each entry
matches an exact connected source-port display name, MIDI channel, and Control
Change controller number, then replaces only the controller number. The MIDI
channel and value are preserved. Mappings are applied once without chaining;
unmatched Control Change and all other MIDI messages remain unchanged.

The source-port name is the stable, human-readable name printed by Zeta:

```text
[MIDI input] connected: SE49 MIDI2
```

Do not use the numeric ALSA address shown by tools such as `aseqdump`; that
address may change after a reboot or reconnection. YAML channels use the
human-facing range 1 through 16, and controller numbers range from 0 through
127.

The example mapping turns the SE49 MIDI2 fader event on channel 16 from CC20
into standard Channel Volume CC7 while MMC transport mode remains enabled.
The resulting CC7 follows Zeta's existing routing: it controls the live channel
in Ready and Looping, and the pending-loop channel in Armed and Recording.

### Controller bindings

`controls.recording`, `controls.next_soundfont`, `controls.octave_down`, and
`controls.octave_up` each require exactly one binding. A matched control event
is reserved for the action: it does not sound and is not recorded. Actions may
not use overlapping bindings. Edit a binding before the performance when the
physical control setup changes.

YAML MIDI channels use the human-facing range 1 through 16.

MIDI note:

```yaml
controls:
  recording:
    type: note
    channel: 1
    key: 84
```

This matches a positive-velocity Note On. That piano key becomes a dedicated
control, so it is not recommended when the complete keyboard must remain
playable.

Control Change:

```yaml
controls:
  recording:
    type: control_change
    channel: 1
    controller: 64
    value: 127
```

Do not bind CC64 when the sustain pedal must remain available.

Exact Program Change:

```yaml
controls:
  recording:
    type: program_change
    channel: 1
    program: 12
```

Any Program Change on a channel:

```yaml
controls:
  recording:
    type: program_change
    channel: 1
    program: any
```

MIDI Machine Control (MMC):

```yaml
controls:
  recording:
    type: machine_control
    command: rewind
```

MMC bindings have no MIDI channel. Supported command names are `stop`, `play`,
`deferred_play`, `fast_forward`, `rewind`, `record_strobe`, `record_exit`,
`record_pause`, `pause`, `eject`, `chase`, and `reset`.

## Nektar SE49 example

The SE49 can assign its four Octave and Transpose buttons to MMC transport
commands. In that mode, Transpose Down sends Rewind, Transpose Up sends Stop,
Octave Down sends Play, and Octave Up sends Record Strobe, matching
`zeta.example.yaml`.

Configure MMC and enable transport mode:

1. Press **Octave Up + Transpose Up** simultaneously. The Setup LED should
   blink orange.
2. Press the musical **A2** piano key. `A2` is the note name; it may not have a
   printed setup action on the controller.
3. Press the numeric key **3**.
4. Press **Enter (C5)** to save and leave Setup.
5. Press **Octave Down + Transpose Down** simultaneously to enable MMC
   transport mode.

During a performance:

1. Press **Transpose Up** to select the next SoundFont.
2. Press **Transpose Down** to arm recording with the current SoundFont.
3. Play the first note to begin recording.
4. Press **Transpose Down** again to finish the take and start the loop.
5. Press **Transpose Up** while looping to change the live SoundFont.
6. Use **Octave Down** and **Octave Up** while no notes are playing to shift by
   twelve semitones.

The octave range is three octaves down through four octaves up and does not
wrap. Before recording, octave changes affect both live playing and the pending
loop. Octave changes are ignored while recording. Once looping, they affect
only live playing, so the recorded loop keeps its pitch. Notes whose shifted
key would fall outside MIDI range 0 through 127 retain their original key.

Both Octave LEDs remain on in MMC transport mode because shifting is performed
by Zeta rather than by the controller. Press **Octave Down + Transpose Down**
together again to restore the buttons' native functions.

The setup procedure and control assignments are documented in the
[Nektar SE49/SE61 Owner's Manual](https://support.nektartech.com/wp-content/uploads/my-downloads/Owners_Manuals/SE49_61_printed_guide_v1_3_ENGLISH.pdf).

## Desktop usage

Connect the audio interface and MIDI controller, then run with an explicit
configuration path:

```bash
./build/midi_looper /path/to/zeta.yaml
```

Press Ctrl-C to shut down gracefully. SIGTERM is handled the same way.

With no argument, Zeta reads `/etc/zeta-daw/zeta.yaml`.

## Starting automatically on Raspberry Pi

Install the executable and configuration:

```bash
sudo install -m 0755 build/midi_looper /usr/local/bin/midi_looper
sudo install -d /etc/zeta-daw
sudo install -m 0644 zeta.yaml /etc/zeta-daw/zeta.yaml
```

Use absolute SoundFont paths for a service and ensure the service account can
read them. Give that account access to ALSA devices; on Raspberry Pi OS this
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

Replace `YOUR_USER`, then enable the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now zeta-daw.service
```

The MIDI controller may be connected before or after Zeta starts. Inspect and
control the service with:

```bash
systemctl status zeta-daw.service
journalctl -u zeta-daw.service -f
sudo systemctl restart zeta-daw.service
sudo systemctl stop zeta-daw.service
```

## Troubleshooting

If configuration fails, follow the reported YAML location and field name. For
a SoundFont error, verify the path and its readability by the desktop or
service user.

If controller actions do not work, stop Zeta and inspect the actual events:

```bash
aseqdump -l
aseqdump -p CLIENT:PORT
```

Replace `CLIENT:PORT` with the controller port shown by `aseqdump -l`. For the
SE49 example, all four Octave and Transpose buttons should emit MMC SysEx
messages. If they do not, confirm that MMC transport mode is enabled.

Selection is also reported as `SoundFont selected: ID`. If live notes or the
loop use an unexpected sound, compare that log with the order and bank/preset
values under `soundfonts`.

Development workflow and architecture are documented in
[CONTRIBUTING.md](CONTRIBUTING.md).
