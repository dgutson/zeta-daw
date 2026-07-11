#include "fake_fluidsynth.hpp"

#include <array>
#include <condition_variable>
#include <mutex>
#include <utility>

struct _fluid_hashtable_t {};

struct _fluid_synth_t {
    struct Program {
        int soundfont_id{-1};
        int bank{-1};
        int preset{-1};
    };

    std::array<Program, 16> programs;
};

struct _fluid_audio_driver_t {};

struct _fluid_midi_event_t {
    int type{};
    int channel{};
    int key{};
    int velocity{};
    int control{};
    int value{};
    int program{};
    int pitch{};
};

struct _fluid_midi_driver_t {
    handle_midi_event_func_t handler{};
    void* data{};
};

namespace {

std::mutex mutex;
std::condition_variable calls_changed;
std::vector<fake_fluidsynth::Call> recorded_calls;
_fluid_midi_driver_t* active_midi_driver{};
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
    active_midi_driver = nullptr;
    next_soundfont_id = 1;
}

int emitMidi(const MidiEvent& source) {
    handle_midi_event_func_t handler = nullptr;
    void* data = nullptr;

    {
        std::lock_guard lock(mutex);
        if (active_midi_driver) {
            handler = active_midi_driver->handler;
            data = active_midi_driver->data;
        }
    }

    if (!handler) {
        return FLUID_FAILED;
    }

    _fluid_midi_event_t event{
        .type = source.type,
        .channel = source.channel,
        .key = source.key,
        .velocity = source.velocity,
        .control = source.control,
        .value = source.value,
        .program = source.program,
        .pitch = source.pitch,
    };

    return handler(data, &event);
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

int fluid_settings_setint(fluid_settings_t*, const char*, int) {
    return 1;
}

fluid_synth_t* new_fluid_synth(fluid_settings_t*) {
    return new _fluid_synth_t;
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

fluid_midi_driver_t* new_fluid_midi_driver(
    fluid_settings_t*,
    handle_midi_event_func_t handler,
    void* data
) {
    auto* driver = new _fluid_midi_driver_t{
        .handler = handler,
        .data = data,
    };

    {
        std::lock_guard lock(mutex);
        active_midi_driver = driver;
    }
    return driver;
}

void delete_fluid_midi_driver(fluid_midi_driver_t* driver) {
    {
        std::lock_guard lock(mutex);
        if (active_midi_driver == driver) {
            active_midi_driver = nullptr;
        }
    }
    record({
        .kind = fake_fluidsynth::CallKind::DeleteMidiDriver,
        .text = {},
    });
    delete driver;
}

int fluid_midi_event_get_type(const fluid_midi_event_t* event) {
    return event->type;
}

int fluid_midi_event_set_channel(fluid_midi_event_t* event, int channel) {
    event->channel = channel;
    return FLUID_OK;
}

int fluid_midi_event_get_channel(const fluid_midi_event_t* event) {
    return event->channel;
}

int fluid_midi_event_get_key(const fluid_midi_event_t* event) {
    return event->key;
}

int fluid_midi_event_get_velocity(const fluid_midi_event_t* event) {
    return event->velocity;
}

int fluid_midi_event_get_control(const fluid_midi_event_t* event) {
    return event->control;
}

int fluid_midi_event_get_value(const fluid_midi_event_t* event) {
    return event->value;
}

int fluid_midi_event_get_program(const fluid_midi_event_t* event) {
    return event->program;
}

int fluid_midi_event_get_pitch(const fluid_midi_event_t* event) {
    return event->pitch;
}

int fluid_synth_handle_midi_event(void*, fluid_midi_event_t* event) {
    record({
        .kind = fake_fluidsynth::CallKind::HandleMidi,
        .type = event->type,
        .channel = event->channel,
        .key = event->key,
        .velocity = event->velocity,
        .control = event->control,
        .value = event->value,
        .text = {},
    });
    return FLUID_OK;
}

int fluid_synth_noteon(fluid_synth_t*, int channel, int key, int velocity) {
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
        .kind = fake_fluidsynth::CallKind::SynthNoteOff,
        .channel = channel,
        .key = key,
        .text = {},
    });
    return FLUID_OK;
}

int fluid_synth_cc(fluid_synth_t*, int channel, int control, int value) {
    record({
        .kind = fake_fluidsynth::CallKind::SynthControlChange,
        .channel = channel,
        .control = control,
        .value = value,
        .text = {},
    });
    return FLUID_OK;
}

} // extern "C"
