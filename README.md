# Zeta DAW MIDI Looper

Zeta DAW is a Linux-only C++20 MIDI looper, including Raspberry Pi 5. Its ALSA
MIDI input and POSIX lifecycle integration do not support macOS or Windows. It
can run as a desktop process or start automatically as a headless service and
be controlled entirely from a MIDI controller.

The loop-slot performance workflow is:

1. Press the configured loop-slot control, then the first configured slot key to
   arm the guide.
2. Play the first guide note. Recording starts on that note, with no leading
   silence.
3. Press the loop-slot control alone to finish the guide take and start it
   looping; the slot key is not repeated.
4. Arm any later slot and record at any point in the guide. Its first note fixes
   its phase, and its arbitrary-length phrase repeats on whole guide cycles.
5. Keep adding synchronized slots or playing live over them.

The guide must be looping before any later slot can be armed. From Ready,
selecting a Looping regular slot stops only that slot; selecting the guide stops
and discards every regular slot before stopping the guide. Selecting a stopped
slot arms a replacement rather than resuming its old take. Pressing the
loop-slot control while Armed cancels and discards the pending take. Exit is
only through Ctrl-C, SIGTERM, or another process shutdown signal.

## Requirements and installation

- Linux, including Raspberry Pi OS or Ubuntu; macOS and Windows are not
  supported
- A C++20-capable C++ compiler; GCC is the currently CI-tested toolchain
- CMake 3.22 or newer
- pkg-config
- ALSA, PulseAudio, and libsndfile development files
- FluidSynth 2.5.7 fetched by CMake; libremidi 5.4.3 and yaml-cpp installed or
  fetched by CMake
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
    libpulse-dev \
    libsndfile1-dev \
    libyaml-cpp-dev
```

ALSA, PulseAudio, and libsndfile are required system dependencies. CMake
downloads checksum-verified FluidSynth 2.5.7 and links it statically into
`zd` and `zfont`; an installed distro FluidSynth is neither required nor used.
The pinned build supports ALSA and PulseAudio output plus SF2 and SF3
SoundFonts. CMake first looks for libremidi 5.4.3 and yaml-cpp, then downloads
pinned copies when they are not available. Test builds also fetch pinned Hegel
and GoogleTest dependencies. The yaml-cpp fallback supports both the minimum
CMake 3.22 line and CMake 4.x despite that dependency's older policy metadata.
The first build therefore requires network access when fetched dependencies are
not already cached.

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

The script downloads the pinned dependencies on the first build and produces
the release executable at `build/zd`. It explicitly disables MIDI tracing,
even when the build directory previously cached another value. Release builds
compile both Zeta and FluidSynth with the compiler's release optimizations and
`-march=native`; with the supported GCC toolchain this includes `-O3`. Build
the deployment executable on the desktop or Raspberry Pi where it will run.
An ordinary single-configuration CMake build also defaults to Release when
`CMAKE_BUILD_TYPE` is not specified.

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
schema_version: 8

# Optional reliability-first audio settings found while resolving issue 23.
# Leave this mapping commented out to preserve FluidSynth's defaults and
# Zeta's existing gain of 0.5.
# audio:
#   driver: alsa
#   alsa_device: plughw:CARD=Device,DEV=0
#   gain: 1.0
#   period_size: 256
#   periods: 8

loop_slots:
  - C2
  - D2

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

Only schema version 8 is accepted. A configuration error stops startup and
reports the invalid field.

### Audio output

`audio` and each of its fields are optional. When omitted, FluidSynth chooses
its default driver, device, period size, and period count; Zeta retains its
existing gain of `0.5`. With the pinned FluidSynth 2.5.7 on Linux, the current
FluidSynth buffer defaults are 64 frames and 16 periods. Zeta deliberately
does not duplicate those platform defaults. `alsa_device` requires
`driver: alsa`:

- `driver`: a non-empty FluidSynth audio-driver name
- `alsa_device`: a non-empty ALSA PCM device string
- `gain`: FluidSynth master gain from `0.0` through `10.0`
- `period_size`: optional frames per render call from `64` through `8192`
- `periods`: optional driver buffer count from `2` through `64`

For a Raspberry Pi USB sound card, list hardware cards and PCM device names:

```bash
aplay -l
aplay -L
```

Then configure the card, for example:

```yaml
audio:
  driver: alsa
  alsa_device: plughw:CARD=Device,DEV=0
  gain: 1.0
  period_size: 256
  periods: 8
```

`Device` is the ALSA card ID in this example; use the exact ID that `aplay -L`
reports for the intended output. `hw:CARD=Device,DEV=0` opens the hardware
device directly and succeeds only when the requested sample format and rate
are supported exactly. `plughw:CARD=Device,DEV=0` adds ALSA's conversion layer
and is generally the more compatible choice. Prefer this stable named form
because numeric card indexes can change across reboots and USB reconnections.

Gain `1.0` is valid, but it provides substantially more level than Zeta's
default `0.5` and can clip when several voices or loop slots sound together.
Use only the gain needed by the output chain.

#### Determining audio-buffer values

Leave both settings unset when FluidSynth's defaults play cleanly. Override
them only when the intended output device produces intermittent clicks or when
its latency needs tuning. The audio driver may negotiate nearby values when a
device cannot provide the exact request.

`period_size` is the number of samples FluidSynth renders in one operation. A
larger block gives FluidSynth more time to meet each rendering deadline and
reduces how often it must wake up, which can prevent clicks. The performer-side
cost is concrete: a note, pedal movement, or other newly received MIDI event
may not affect the sound until the next block, so larger blocks can make those
changes start later and with more timing variation.

`periods` controls how many such blocks the audio driver may buffer. More
periods let playback survive a longer temporary operating-system scheduling
delay, but may increase the delay between performing an action and hearing its
result. These are independent controls, so Zeta exposes both rather than a
single low/medium/high setting.

At sample rate `R`, render granularity is `1000 × period_size / R`
milliseconds and the configured maximum buffer is
`1000 × period_size × periods / R` milliseconds. For 256 × 8 at 44.1 kHz,
those values are about 5.8 ms and 46.4 ms.

For a Raspberry Pi 5 used for live performance, start with the explicit
reliability-first 256 × 8 values shown above. This recommendation still needs
acceptance testing with the intended USB sound card; it is not a universal
hardware default.

Tune one field at a time with that real output device and the largest expected
performance load:

1. Configure 256 × 8 and run the stress profile:

   ```bash
   ./build/zsoundtest --stress /path/to/zeta.yaml
   ```

2. Run `zd` for an extended session while playing live over the maximum
   expected number of active loops. The synthetic diagnostic does not model
   every application worker or operating-system scheduling delay.
3. If brief clicks or frying remain, first increase `periods` from 8 to 16.
   This adds scheduling headroom without making MIDI-event timing coarser. If
   that is still insufficient, try `period_size: 512`.
4. If playback is clean but the instrument responds too slowly, reduce
   `periods` from 8 to 4, then to 2, testing after each change. Keep
   `period_size` at 256 during this comparison. Only then try reducing it to
   128, because smaller render calls increase deadline pressure and 128
   produced a likely click during the issue-23 stress test.
5. Accept the smallest clean combination only after repeated worst-case live
   sessions. A single click rejects a candidate; also confirm that live
   controller timing still feels natural.

`zsoundtest` reports FluidSynth's effective settings when it starts, and
FluidSynth warns if the audio driver substitutes values. Raspberry Pi
acceptance must use the same USB sound card, ALSA device string, SoundFonts,
loop count, and service limits intended for performance.

### Loop slots

`loop_slots` is an ordered, non-empty catalog of physical note names using the
same convention described below for direct SoundFont selection. The first
entry is the guide and every later entry is a regular slot. Catalog order
defines the stable loop-slot identity; live output uses FluidSynth channel 0
and loop slots use independent channels starting at 1.

Press `controls.loop_slot_by_note`, then a slot key. Selection uses the raw key
before Zeta octave transposition and ignores incoming MIDI channel. The note is
consumed and never sounds or enters a take. Slot keys must be unique and cannot
reuse a configured Note action. They may overlap SoundFont keys because the two
selector controls disambiguate the gesture.

A Muted slot has no resumable take. Selecting it clears any previous take and
arms replacement recording, except that a regular slot cannot arm until the
guide is looping. The guide period runs from its first recorded note to the
completion control. A regular slot's first note captures its phase within that
guide period. Its phrase may span any number of guide cycles and repeats at the
smallest whole guide multiple that contains its musical content. Those natural
repetitions begin one whole phrase period after the first recorded note. On
completion, playback joins the current natural repetition when one is already
in progress, omitting only its elapsed event prefix once; otherwise it waits
for the first natural repetition. Later repetitions play the complete phrase,
and completion-button timing does not make synchronized slots drift.

Silence after the final released note and before completion is not part of a
regular phrase. Notes still held at completion are ended cleanly at that point.
Selecting a Looping regular slot stops and discards only that slot. Selecting
the Looping guide stops and discards every regular slot first, then the guide;
a replacement guide establishes a new timeline. Slots remain unquantized
within the guide cycle: synchronization preserves the musician's recorded
phase rather than snapping notes to a beat grid.

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

To inspect and audition every preset in a SoundFont, build and run `zfont`:

```bash
./build/zfont /path/to/piano.sf2 /path/to/zeta.yaml
```

The configuration path is optional and defaults to
`/etc/zeta-daw/zeta.yaml`. `zfont` uses its FluidSynth audio settings, prints
each preset as `bank-preset name`, plays C3 for one second, and immediately
continues to the next preset. The inspected `.sf2` or `.sf3` file does not need
to appear in the configuration.

To investigate intermittent clicks or sparks with repeatable input, run the
interactive `zsoundtest` diagnostic:

```bash
./build/zsoundtest
```

Stop the regular Zeta process or service first so it cannot share the output or
add live notes during the diagnostic. The tool reads
`/etc/zeta-daw/zeta.yaml` by default, uses the configured audio output, and
lists every configured SoundFont preset before creating the audio driver.
Choose one preset or all presets. For each selection it tests MIDI keys 36 and
84 in both directions, at velocity 110, using slow-separated,
rapid-separated, rapid-immediate, and rapid-overlap transitions. It repeats
the matrix at the configured gain and at FluidSynth's conservative gain of
`0.2`. After each labeled case, report clean, spark, or unsure; the final
summary identifies every reported condition and its observed peak voice count.
Natural release tails remain audible while the result prompt waits and are not
forcibly cut before the next case, so wait until the tail is silent before
submitting the result. Replay, skip-preset, and quit controls are available at
every result prompt.

To inspect the complete matrix without opening an audio device:

```bash
./build/zsoundtest --list
```

Pass a configuration path after the executable, or after `--list`, to override
the default.

After a matrix result identifies a suspect preset, isolate note attacks and
releases with:

```bash
./build/zsoundtest --single
```

Choose the suspect preset. The focused mode plays keys 84 and 36 individually
at each test gain and records whether a click occurred at the attack or release
of each note.

To repeat the same focused test with a larger rendering period, run:

```bash
./build/zsoundtest --single-large-period
```

This diagnostic-only mode overrides the configured period size with 512
frames. It reduces rendering wake-ups and provides more time per render call at
the cost that newly received notes and controls may take longer to affect the
sound. It does not change the YAML file.

To deliberately increase FluidSynth rendering pressure while keeping the
audible probe at one note, run the deterministic stress profile with the
effective buffer settings (YAML overrides when present, otherwise FluidSynth's
defaults):

```bash
./build/zsoundtest --stress
```

The profile holds quiet background voices across progressively more MIDI
channels while repeatedly playing key 36 without per-note terminal output. It
reports peak active voices and FluidSynth CPU load, then records clicks/frying
and sustained `RRRR` separately. Compare the same profile with a 512-frame
period size using:

```bash
./build/zsoundtest --stress-large-period
```

The override profiles retain the effective period count from the YAML or
FluidSynth default. To compare smaller render periods, use:

```bash
./build/zsoundtest --stress-period-256
./build/zsoundtest --stress-period-128
```

At 44.1 kHz, periods of 128, 256, and 512 frames provide about 2.9, 5.8,
and 11.6 milliseconds per render call, respectively.

To reproduce the constant-buffer 256 × 4 experiment, run:

```bash
./build/zsoundtest --stress-period-256-periods-4
```

This overrides both YAML buffer fields and contains 1,024 configured frames.

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
   guide continues. Start whenever the phrase should enter; that phase is
   preserved on every repetition.
6. From Ready, press **Transpose Down**, then **D2** to stop only the regular
   slot. Press **Transpose Down**, then **C2** to stop the guide and every
   regular slot. Repeat the guide gesture to arm its replacement.
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

Use a dedicated, non-root service account. The examples below use
`YOUR_USER`; replace it with that account name. Install the executable,
configuration, and SoundFonts at paths the account can traverse and read:

```bash
sudo install -m 0755 build/zd /usr/local/bin/zd
sudo install -d -m 0755 /etc/zeta-daw
sudo install -m 0644 zeta.yaml /etc/zeta-daw/zeta.yaml
sudo install -d -m 0755 /usr/local/share/zeta-daw/soundfonts
sudo install -m 0644 /path/to/piano.sf2 \
    /usr/local/share/zeta-daw/soundfonts/piano.sf2
```

Use the installed absolute SoundFont paths in `/etc/zeta-daw/zeta.yaml`. Add
the service account to the `audio` group so it can open ALSA audio and MIDI
devices, then verify its membership:

```bash
sudo usermod -aG audio YOUR_USER
id YOUR_USER
```

Group membership and readable files are required for the service. Real-time
scheduling and locked memory are optional performance hardening: Zeta can
produce audio without them, but FluidSynth may warn that it could not acquire
real-time priority or lock sample memory, and a heavily loaded system may be
more prone to audio dropouts.

For an interactive desktop launch, PAM limits can grant those optional
resources. Create `/etc/security/limits.d/zeta-audio.conf` with:

```text
@audio - rtprio 90
@audio - memlock unlimited
```

The filename must end in `.conf`. Log out completely and back in after changing
PAM limits. These login-session limits do not configure a system service.
`rtprio 90` is above FluidSynth's default audio-thread priority of 60.
Unlimited locked memory avoids a SoundFont-size-dependent limit, but it lets
the process lock any amount of RAM; reserve it for a dedicated performance
account.

Create `/etc/systemd/system/zeta-daw.service`:

```ini
[Unit]
Description=Zeta DAW MIDI looper
After=sound.target
# Keep retrying if a configured USB audio device appears late during boot.
StartLimitIntervalSec=0

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

`User`, `SupplementaryGroups`, `ExecStart`, the restart policy, and the
`multi-user.target` installation target provide the unattended startup
contract. `After=sound.target` orders Zeta after system sound initialization,
but it cannot guarantee that a particular USB device is already present;
unlimited retries cover that early-boot race. No graphical target or
interactive login is required.

For optional real-time hardening of the system service, add these directives
under `[Service]`:

```ini
LimitRTPRIO=90
LimitMEMLOCK=infinity
```

Unlike PAM login limits, these directives apply directly to the service.
`LimitRTPRIO` allows FluidSynth to request its real-time audio-thread priority.
`LimitMEMLOCK=infinity` allows FluidSynth to keep its SoundFont samples locked
instead of subject to paging, at the cost of removing systemd's locked-memory
cap for this process. Apply them after basic audio startup works so permission
or device failures remain distinct from performance tuning.

Enable and start the service, then verify both enablement and the current boot:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now zeta-daw.service
systemctl is-enabled zeta-daw.service
systemctl status zeta-daw.service
journalctl -u zeta-daw.service -b
```

`is-enabled` must report `enabled`, and status must report `active (running)`.
Reboot without logging in, then repeat the three verification commands. The
MIDI controller may be connected before or after Zeta starts; Zeta discovers
and reconnects it automatically.

To follow logs or control the service later:

```bash
journalctl -u zeta-daw.service -f
sudo systemctl restart zeta-daw.service
sudo systemctl stop zeta-daw.service
```

## Optimizing Raspberry Pi for headless operation

Use a desktop-capable Raspberry Pi OS image so the graphical environment is
available for maintenance. Complete the automatic Zeta service setup above
first. Existing user and login settings are left unchanged.

### Boot without the graphical environment

Make the non-graphical systemd target the default, then reboot:

```bash
sudo systemctl set-default multi-user.target
sudo reboot
```

After reboot, confirm that the Pi remains headless and Zeta is running:

```bash
systemctl get-default
systemctl status zeta-daw.service
```

`get-default` must report `multi-user.target`.

### Start the graphical environment once

Connect a screen, keyboard, and mouse. Using the existing local login, start
the graphical environment for the current boot:

```bash
sudo systemctl isolate graphical.target
```

This does not change the default for the next boot. To stop the graphical
environment again, save any open work and run:

```bash
sudo systemctl isolate multi-user.target
```

Confirm that Zeta remains active after either transition:

```bash
systemctl status zeta-daw.service
```

### Disable onboard Bluetooth

Skip this step if Bluetooth is used for MIDI, a keyboard, or a mouse. USB MIDI
is unaffected.

Open `/boot/firmware/config.txt` with `sudoedit` and add the documented
[`disable-bt` overlay](https://github.com/raspberrypi/firmware/blob/master/boot/overlays/README)
under `[all]`:

```ini
[all]
dtoverlay=disable-bt
```

Disable the Bluetooth service and reboot:

```bash
sudo systemctl disable --now bluetooth.service
sudo reboot
```

After reboot, check that Bluetooth is absent and Zeta still works with USB
audio and MIDI:

```bash
rfkill list
systemctl status zeta-daw.service
```

To restore Bluetooth, remove the `dtoverlay=disable-bt` line, then run:

```bash
sudo systemctl enable bluetooth.service
sudo reboot
```

### Check the result

Run these commands before and after the changes to compare boot time, slow
units, failures, and idle memory use:

```bash
systemd-analyze time
systemd-analyze blame
systemctl --failed
free -h
```

For shutdown delays, inspect the end of the previous boot with
`journalctl -b -1 -e`. Disable another service only after confirming that this
instrument does not use it. Keep the Zeta, audio, MIDI, local-login, and logging
services.

## Troubleshooting

If configuration fails, follow the reported YAML location and field name. For
a SoundFont error, verify the path and its readability by the desktop or
service user.

At startup, Zeta reports `FluidSynth runtime version: 2.5.7`. Because the
renderer is linked into the executable, this reports the version actually in
use rather than a separately installed command-line package.

If FluidSynth reports that the default audio device is in use, stop the
competing application or configure `audio.driver: alsa` and an explicit
`audio.alsa_device` from `aplay -L`. During boot, a nonexistent or late USB
audio device makes startup fail; confirm the configured device with `aplay -L`
and inspect the current boot with `journalctl -u zeta-daw.service -b`. The
documented restart policy keeps trying every two seconds after the device
appears. If systemd reports `start-limit-hit`, confirm that
`StartLimitIntervalSec=0` is present under `[Unit]`, run `sudo systemctl
daemon-reload`, and reset the failed state with `sudo systemctl reset-failed
zeta-daw.service`.

If FluidSynth warns that it could not set high priority or lock memory, audio
can still start. For an interactive process, inspect the PAM limits; for the
system service, inspect `LimitRTPRIO` and `LimitMEMLOCK` in the unit. PAM login
limits do not apply to the system service.

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
