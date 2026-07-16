# Zeta DAW MIDI Looper

Zeta DAW is a Linux-only C++20 MIDI looper, including Raspberry Pi 5. Its ALSA
MIDI input and POSIX lifecycle integration do not support macOS or Windows. It
can run as a desktop process or start automatically as a headless service and
be controlled entirely from a MIDI controller.

The loop-slot performance workflow is:

1. Press the configured loop-slot control, then a configured raw physical note.
2. If that loop slot is Muted, it is cleared and armed for a new take.
3. Play the first recording note. Recording starts on that note, with no leading
   silence.
4. Press the loop-slot control alone to finish the take and start that slot
   looping; the slot note is not repeated.
5. Repeat with another loop slot while earlier slots continue, and keep playing
   live over all of them.

From Ready, selecting a Looping slot stops only that slot. Selecting the stopped
slot again arms a replacement rather than resuming its old take. Pressing the
loop-slot control while Armed cancels and discards the pending take. Exit is only
through Ctrl-C, SIGTERM, or another process shutdown signal.

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
schema_version: 7

loop_slots:
  - { key: C2 }
  - { key: D2 }

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
  loop_slot_by_note:
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

Only schema version 7 is accepted. A configuration error stops startup and
reports the invalid field.

### Loop slots

`loop_slots` is an ordered, non-empty catalog. Each entry has one `key`, using
the same physical note-name convention described below for direct SoundFont
selection. Catalog order defines the stable loop-slot identity; live output
uses FluidSynth channel 0 and loop slots use independent channels starting at
1.

Press `controls.loop_slot_by_note`, then a slot key. Selection uses the raw key
before Zeta octave transposition and ignores incoming MIDI channel. The note is
consumed and never sounds or enters a take. Slot keys must be unique and cannot
reuse a configured Note action. They may overlap SoundFont keys because the two
selector controls disambiguate the gesture.

A Muted slot has no resumable take. Selecting it clears any previous take and
arms replacement recording. Selecting a Looping slot stops only that slot and
discards its take. Zero or more slots can loop concurrently, including while a
different slot is Armed or Recording; loops start when completed and remain
free-running without synchronization or quantization.

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

Arming a loop slot snapshots the current live SoundFont. Next may change that
slot's pending selection before the first note. During Recording, Next is
ignored. Completing or canceling adopts the pending selection for live output;
already looping slots keep their independently locked SoundFonts.

### Direct SoundFont selection by note

Add an optional `key` to each directly selectable entry in `soundfonts`. Press
`controls.soundfont_by_note`, then press that positive-velocity physical key to
select the SoundFont. Keys use the octave convention documented by the SE49
manual, with sharps such as `G3` or `C#4`; MIDI key 60 is `C3`. The convention
is supported across the one-digit MIDI domain `C0` through `G8` (MIDI keys 24
through 127), so equivalent controllers are not restricted to the SE49 keybed.

Selection matches the raw key emitted by the controller before Zeta octave
transposition and is independent of the incoming MIDI channel. This keeps the
key fixed when Zeta's performance octave or the controller's transmit channel
changes. The controller's own stored octave or transpose setting changes the
emitted key and therefore moves the binding; clear those settings before a
performance. Configured SoundFont keys must be unique. A SoundFont key may not
reuse the physical key of any configured Note action, regardless of that
action's MIDI channel.

The selector is one-shot. Pressing it again cancels. The selection note is
consumed and does not sound; an unmapped note is also consumed, reports an
error, changes nothing, and leaves selection mode. All notes remain ordinarily
playable when the selector is not armed.

In Ready, direct selection changes the live SoundFont. In Armed it
changes the pending loop SoundFont without sounding the selection note or
starting recording; the following positive-velocity note starts the take at
offset zero. The selector is ignored during Recording. Every completed slot's
SoundFont remains locked while that slot loops.

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
The resulting CC7 follows Zeta's routing: it controls the live channel in Ready
and the selected slot's channel in Armed and Recording. Other slots continue
on their independent channels.

### Controller bindings

`controls.loop_slot_by_note`, `controls.octave_down`, and `controls.octave_up`
each require exactly one binding. `controls.next_soundfont` and
`controls.soundfont_by_note` are individually optional, with at least one
required. A matched control event is reserved for the action: it does not sound
and is not recorded. Actions may not use overlapping bindings. Edit a binding
before the performance when the physical control setup changes.

YAML MIDI channels use the human-facing range 1 through 16.

MIDI note:

```yaml
controls:
  loop_slot_by_note:
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
  loop_slot_by_note:
    type: control_change
    channel: 1
    controller: 64
    value: 127
```

Do not bind CC64 when the sustain pedal must remain available.

Exact Program Change:

```yaml
controls:
  loop_slot_by_note:
    type: program_change
    channel: 1
    program: 12
```

Any Program Change on a channel:

```yaml
controls:
  loop_slot_by_note:
    type: program_change
    channel: 1
    program: any
```

MIDI Machine Control (MMC):

```yaml
controls:
  loop_slot_by_note:
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

Configure MMC, clear the controller's stored pitch offsets, and enable
transport mode:

1. Press **Octave Up + Transpose Up** simultaneously. The Setup LED should
   blink orange.
2. Press the musical **A2** piano key. `A2` is the note name; it may not have a
   printed setup action on the controller.
3. Press the numeric key **3**.
4. Press **Enter (C5)** to save and leave Setup.
5. Re-enter Setup, press the low **F#1** key for Transpose, enter numeric
   **0**, and press **Enter (C5)**.
6. Re-enter Setup, press the low **G1** key for Octave, enter numeric **0**,
   and press **Enter (C5)**.
7. Press **Octave Down + Transpose Down** simultaneously to enable MMC
   transport mode.

Use the explicit Setup entries in steps 5 and 6. The Transpose button-pair
reset works only while the buttons retain their native Transpose assignment,
and a stored pitch offset would make a configured key such as `G3` arrive as a
different MIDI key.

With `zeta.example.yaml`, during a performance:

1. Press **Transpose Up**, then **G3** for piano or **A3** for bass.
2. Press **Transpose Down**, then **C2** to arm the first loop slot with the
   current SoundFont.
3. Play the first note to begin recording.
4. Press **Transpose Down** alone to finish the take and start that slot.
5. Press **Transpose Down**, then **D2** to record the second slot while the
   first continues.
6. From Ready, press **Transpose Down**, then **C2** to stop only the first
   slot. Repeat that gesture to arm its replacement.
7. Use **Transpose Up** plus a SoundFont key for live selection, and use
   **Octave Down** or **Octave Up** while no notes are playing to shift by
   twelve semitones.

The octave range is three octaves down through four octaves up and does not
wrap. Arming snapshots the live octave into that loop slot; changes while Armed
affect live playing and the pending slot. Octave changes are ignored while
Recording. Once a slot loops, later changes affect only live playing, so every
recorded loop keeps its pitch. Notes whose shifted key would fall outside MIDI
range 0 through 127 retain their original key.

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
