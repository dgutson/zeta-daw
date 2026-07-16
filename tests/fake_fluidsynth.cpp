#include "fake_fluidsynth.hpp"

#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <utility>

struct _fluid_hashtable_t {
    int midi_channels{16};
};

struct _fluid_synth_t {
    struct Program {
        int soundfont_id{-1};
        int bank{-1};
        int preset{-1};
    };

    explicit _fluid_synth_t(int midi_channels)
        : programs(static_cast<std::size_t>(midi_channels)) {}

    std::vector<Program> programs;
};

struct _fluid_audio_driver_t {};

namespace {

std::mutex mutex;
std::condition_variable calls_changed;
std::vector<fake_fluidsynth::Call> recorded_calls;
int next_soundfont_id{1};

void record(fake_fluidsynth::Call call) {
    {
        std::lock_guard lock(mutex);
        recorded_calls.push_back(std::move(call));
    }
    calls_changed.notify_all();
}

} // namespace

namespace fake_fluidsynth {

void reset() {
    std::lock_guard lock(mutex);
    recorded_calls.clear();
    next_soundfont_id = 1;
}

std::vector<Call> calls() {
    std::lock_guard lock(mutex);
    return recorded_calls;
}

bool waitUntil(
    const std::function<bool(const std::vector<Call>&)>& predicate,
    std::chrono::milliseconds timeout
) {
    std::unique_lock lock(mutex);
    return calls_changed.wait_for(lock, timeout, [&] {
        return predicate(recorded_calls);
    });
}

} // namespace fake_fluidsynth

extern "C" {

fluid_settings_t* new_fluid_settings() {
    return new _fluid_hashtable_t;
}

void delete_fluid_settings(fluid_settings_t* settings) {
    record({
        .kind = fake_fluidsynth::CallKind::DeleteSettings,
        .text = {},
    });
    delete settings;
}

int fluid_settings_setstr(fluid_settings_t*, const char*, const char*) {
    return 1;
}

int fluid_settings_setnum(fluid_settings_t*, const char*, double) {
    return 1;
}

int fluid_settings_setint(
    fluid_settings_t* settings,
    const char* name,
    int value
) {
    if (std::strcmp(name, "synth.midi-channels") == 0) {
        settings->midi_channels = value;
        record({
            .kind = fake_fluidsynth::CallKind::ConfigureMidiChannels,
            .value = value,
            .text = name,
        });
    }
    return 1;
}

fluid_synth_t* new_fluid_synth(fluid_settings_t* settings) {
    return new _fluid_synth_t{settings->midi_channels};
}

void delete_fluid_synth(fluid_synth_t* synth) {
    record({
        .kind = fake_fluidsynth::CallKind::DeleteSynth,
        .text = {},
    });
    delete synth;
}

int fluid_synth_sfload(fluid_synth_t*, const char* filename, int) {
    int soundfont_id = 0;
    {
        std::lock_guard lock(mutex);
        soundfont_id = next_soundfont_id++;
    }

    record({
        .kind = fake_fluidsynth::CallKind::LoadSoundFont,
        .soundfont_id = soundfont_id,
        .text = filename,
    });
    return soundfont_id;
}

int fluid_synth_program_select(
    fluid_synth_t* synth,
    int channel,
    int soundfont_id,
    int bank,
    int preset
) {
    synth->programs.at(static_cast<std::size_t>(channel)) = {
        .soundfont_id = soundfont_id,
        .bank = bank,
        .preset = preset,
    };

    record({
        .kind = fake_fluidsynth::CallKind::SelectProgram,
        .channel = channel,
        .soundfont_id = soundfont_id,
        .bank = bank,
        .preset = preset,
        .text = {},
    });
    return FLUID_OK;
}

int fluid_synth_get_program(
    fluid_synth_t* synth,
    int channel,
    int* soundfont_id,
    int* bank,
    int* preset
) {
    const auto& program = synth->programs.at(static_cast<std::size_t>(channel));
    *soundfont_id = program.soundfont_id;
    *bank = program.bank;
    *preset = program.preset;
    return FLUID_OK;
}

fluid_audio_driver_t* new_fluid_audio_driver(fluid_settings_t*, fluid_synth_t*) {
    return new _fluid_audio_driver_t;
}

void delete_fluid_audio_driver(fluid_audio_driver_t* driver) {
    record({
        .kind = fake_fluidsynth::CallKind::DeleteAudioDriver,
        .text = {},
    });
    delete driver;
}

int fluid_synth_noteon(fluid_synth_t*, int channel, int key, int velocity) {
    record({
        .kind = fake_fluidsynth::CallKind::HandleMidi,
        .type = 0x90,
        .channel = channel,
        .key = key,
        .velocity = velocity,
        .text = {},
    });
    record({
        .kind = fake_fluidsynth::CallKind::SynthNoteOn,
        .channel = channel,
        .key = key,
        .velocity = velocity,
        .text = {},
    });
    return FLUID_OK;
}

int fluid_synth_noteoff(fluid_synth_t*, int channel, int key) {
    record({
        .kind = fake_fluidsynth::CallKind::HandleMidi,
        .type = 0x80,
        .channel = channel,
        .key = key,
        .text = {},
    });
    record({
        .kind = fake_fluidsynth::CallKind::SynthNoteOff,
        .channel = channel,
        .key = key,
        .text = {},
    });
    return FLUID_OK;
}

int fluid_synth_cc(fluid_synth_t*, int channel, int control, int value) {
    record({
        .kind = fake_fluidsynth::CallKind::HandleMidi,
        .type = 0xB0,
        .channel = channel,
        .control = control,
        .value = value,
        .text = {},
    });
    record({
        .kind = fake_fluidsynth::CallKind::SynthControlChange,
        .channel = channel,
        .control = control,
        .value = value,
        .text = {},
    });
    return FLUID_OK;
}

int fluid_synth_program_change(fluid_synth_t* synth, int channel, int program) {
    synth->programs.at(static_cast<std::size_t>(channel)).preset = program;
    record({
        .kind = fake_fluidsynth::CallKind::HandleMidi,
        .type = 0xC0,
        .channel = channel,
        .value = program,
        .text = {},
    });
    return FLUID_OK;
}

int fluid_synth_pitch_bend(fluid_synth_t*, int channel, int pitch) {
    record({
        .kind = fake_fluidsynth::CallKind::HandleMidi,
        .type = 0xE0,
        .channel = channel,
        .value = pitch,
        .text = {},
    });
    return FLUID_OK;
}

int fluid_synth_channel_pressure(fluid_synth_t*, int channel, int pressure) {
    record({
        .kind = fake_fluidsynth::CallKind::HandleMidi,
        .type = 0xD0,
        .channel = channel,
        .value = pressure,
        .text = {},
    });
    return FLUID_OK;
}

int fluid_synth_key_pressure(
    fluid_synth_t*,
    int channel,
    int key,
    int pressure
) {
    record({
        .kind = fake_fluidsynth::CallKind::HandleMidi,
        .type = 0xA0,
        .channel = channel,
        .key = key,
        .value = pressure,
        .text = {},
    });
    return FLUID_OK;
}

} // extern "C"
