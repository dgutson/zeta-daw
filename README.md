# Zeta DAW MIDI Looper

Zeta DAW is a Linux-only C++20 MIDI looper, including Raspberry Pi 5. Its ALSA
MIDI input and POSIX lifecycle integration do not support macOS or Windows. It
can run as a desktop process or start automatically as a headless service and
be controlled entirely from a MIDI controller.

The direct-selection performance workflow is:

1. Press the configured SoundFont-by-note control, then a keyed piano note.
2. Press the configured recording control to arm the looper.
3. Play the first recording note. Recording starts on that note, with no leading
   silence.
4. Press the recording control again to finish the take and start looping.
5. Keep playing live and select other SoundFonts without changing the loop.

Pressing the recording control while looping stops playback and returns to
Ready for a new take. Pressing it while armed, before playing a note, cancels
the pending take and also returns to Ready. Exit is only through Ctrl-C,
SIGTERM, or another process shutdown signal.

## Requirements and installation

- Linux, including Raspberry Pi OS or Ubuntu; macOS and Windows are not
  supported
- A C++20-capable C++ compiler; GCC is the currently CI-tested toolchain
- CMake 3.22 or newer
- pkg-config
- FluidSynth and ALSA development files
- libremidi 5.4.3 and yaml-cpp, installed or fetched by CMake
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

FluidSynth and ALSA are required system dependencies. CMake first looks for
libremidi 5.4.3 and yaml-cpp, then downloads pinned copies when they are not
available. Test builds also fetch pinned Hegel and GoogleTest dependencies.
The first configuration therefore requires network access when those fetched
dependencies are not already cached.

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

The release executable is `build/zd`. This script explicitly disables
MIDI tracing, even when the build directory previously cached another value.

For a debug executable with MIDI routing traces enabled, use the separate debug
build directory:

```bash
./build_debug.sh
```

The trace executable is `build-debug/zd`.

## Configuration

Copy the example and edit it for the available SoundFonts and controller:

```bash
cp zeta.example.yaml zeta.yaml
```

A complete configuration looks like this:

```yaml
schema_version: 6

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
    key: G3

  - id: bass
    file: /srv/zeta-daw/soundfonts/electric-bass.sf2
    bank: 0
    preset: 34
    key: A3

controls:
  recording:
    type: machine_control
    command: rewind

  soundfont_by_note:
    type: machine_control
    command: stop

  next_soundfont:
    type: program_change
    channel: 1
    program: 12

  octave_down:
    type: machine_control
    command: play

  octave_up:
    type: machine_control
    command: record_strobe
```

Only schema version 6 is accepted. A configuration error stops startup and
reports the invalid field.

### SoundFonts

`soundfonts` is an ordered, non-empty list. Every entry has:

- `id`: a unique name shown in the selection log
- `file`: an absolute path or a path relative to the YAML file
- `bank`: the SoundFont bank, from 0 through 16383
- `preset`: the preset, from 0 through 127
- `key`: an optional physical keyboard note that selects this SoundFont after
  the SoundFont-by-note control is pressed

Only include the sounds needed for the performance. They are prepared during
startup, and the first entry is selected initially. Next advances through the
list and wraps to the first entry.

The SoundFont selected when recording is armed is used for the loop. Next may
change that selection while armed but before the first note. During recording,
Next is ignored. During loop playback, Next changes the live sound without
changing the recorded loop. Canceling while armed adopts the pending selection
as the live sound before returning to Ready.

### Direct SoundFont selection by note

Add an optional `key` to each directly selectable entry in `soundfonts`. Press
`controls.soundfont_by_note`, then press that positive-velocity physical key to
select the SoundFont. Keys use the SE49 manual's note names with sharps, such
as `G3` or `C#4`; MIDI key 60 is `C3`. The configured range is the physical
SE49 keybed, `C1` through `C5` (MIDI keys 36 through 84).

Selection matches the raw physical key before Zeta octave transposition and is
independent of the incoming MIDI channel. This keeps the key fixed when the
performance octave or the controller's transmit channel changes. Configured
SoundFont keys must be unique. A SoundFont key may not reuse the physical key
of any configured Note action, regardless of that action's MIDI channel.

The selector is one-shot. Pressing it again cancels. The selection note is
consumed and does not sound; an unmapped note is also consumed, reports an
error, changes nothing, and leaves selection mode. All notes remain ordinarily
playable when the selector is not armed.

In Ready and Looping, direct selection changes the live SoundFont. In Armed it
changes the pending loop SoundFont without sounding the selection note or
starting recording; the following positive-velocity note starts the take at
offset zero. The selector is ignored during Recording. The recorded loop's
SoundFont remains locked while Looping.

`controls.soundfont_by_note` is optional, but when configured at least one
SoundFont must have a `key`. Conversely, SoundFont keys are rejected when that
control is absent. `controls.next_soundfont` is also optional, but at least one
of these two selection controls is required. Configure both for direct and
sequential selection, or configure either one alone. SoundFonts without `key`
remain available through sequential selection.

To inspect the banks and presets in a SoundFont, start FluidSynth with the
file, then use its `fonts` and `inst` shell commands. Consult your distribution's
FluidSynth documentation because command-line audio options differ by system.

### MIDI Control Change mappings

`midi_control_change_mappings` is optional. Omit it when the controller needs
no normalization. Each entry matches an exact connected source-port display
name, MIDI channel, and Control Change controller number, then replaces only
the controller number. The MIDI channel and value are preserved. Mappings are
applied once without chaining; unmatched Control Change and all other MIDI
messages remain unchanged.

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

`controls.recording`, `controls.octave_down`, and `controls.octave_up` each
require exactly one binding. `controls.next_soundfont` and
`controls.soundfont_by_note` are individually optional, with at least one
required. A matched control event is reserved for the action: it does not sound
and is not recorded. Actions may not use overlapping bindings. Edit a binding
before the performance when the physical control setup changes.

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

With `zeta.example.yaml`, during a performance:

1. Press **Transpose Up**, then **G3** for piano or **A3** for bass.
2. Press **Transpose Down** to arm recording with the current SoundFont.
3. Play the first note to begin recording.
4. Press **Transpose Down** again to finish the take and start the loop.
5. Press **Transpose Up** and a keyed note while looping to change the live
   SoundFont.
6. Press **Transpose Down** while looping to stop it and return to Ready.
7. Use **Octave Down** and **Octave Up** while no notes are playing to shift by
   twelve semitones.

The octave range is three octaves down through four octaves up and does not
wrap. Before recording, octave changes affect both live playing and the pending
loop. Octave changes are ignored while recording. Once looping, they affect
only live playing, so the recorded loop keeps its pitch. Notes whose shifted
key would fall outside MIDI range 0 through 127 retain their original key.
Returning to Ready preserves the independent live and loop octave selections;
a later take uses the preserved loop selection.

Both Octave LEDs remain on in MMC transport mode because shifting is performed
by Zeta rather than by the controller. Press **Octave Down + Transpose Down**
together again to restore the buttons' native functions.

To retain sequential selection on Transpose Up, bind its MMC Stop event to
`controls.next_soundfont` instead, omit `controls.soundfont_by_note`, and omit
the SoundFont `key` fields. To configure both mechanisms, give `next_soundfont`
a different non-overlapping controller binding.

The setup procedure and control assignments are documented in the
[Nektar SE49/SE61 Owner's Manual](https://support.nektartech.com/wp-content/uploads/my-downloads/Owners_Manuals/SE49_61_printed_guide_v1_3_ENGLISH.pdf).

## Desktop usage

Connect the audio interface and MIDI controller, then run with an explicit
configuration path:

```bash
./build/zd /path/to/zeta.yaml
```

Press Ctrl-C to shut down gracefully. SIGTERM is handled the same way.

With no argument, Zeta reads `/etc/zeta-daw/zeta.yaml`.

## Starting automatically on Raspberry Pi

Install the executable and configuration:

```bash
sudo install -m 0755 build/zd /usr/local/bin/zd
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
ExecStart=/usr/local/bin/zd /etc/zeta-daw/zeta.yaml
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
