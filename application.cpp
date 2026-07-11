#include "application.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <syncstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zeta {
namespace {

constexpr int live_channel = 0;
constexpr int loop_channel = 1;
constexpr int expression_controller = 11;
constexpr std::size_t max_recorded_events = 16384;

struct SettingsDeleter {
    void operator()(fluid_settings_t* settings) const noexcept {
        if (settings) {
            delete_fluid_settings(settings);
        }
    }
};

struct SynthDeleter {
    void operator()(fluid_synth_t* synth) const noexcept {
        if (synth) {
            delete_fluid_synth(synth);
        }
    }
};

struct AudioDriverDeleter {
    void operator()(fluid_audio_driver_t* driver) const noexcept {
        if (driver) {
            delete_fluid_audio_driver(driver);
        }
    }
};

struct MidiDriverDeleter {
    void operator()(fluid_midi_driver_t* driver) const noexcept {
        if (driver) {
            delete_fluid_midi_driver(driver);
        }
    }
};

using FluidSettings = std::unique_ptr<fluid_settings_t, SettingsDeleter>;
using FluidSynth = std::unique_ptr<fluid_synth_t, SynthDeleter>;
using FluidAudioDriver = std::unique_ptr<fluid_audio_driver_t, AudioDriverDeleter>;
using FluidMidiDriver = std::unique_ptr<fluid_midi_driver_t, MidiDriverDeleter>;

} // namespace

struct Application::Impl {
    struct RecordedNoteEvent {
        uint64_t time_ms{};
        int channel{};
        int key{};
        int velocity{};
        RecordedNoteKind kind{RecordedNoteKind::NoteOff};
    };

    struct PlaybackTake {
        std::vector<RecordedNoteEvent> events;
        uint64_t length_ms{};
    };

    FluidSettings settings;
    FluidSynth synth;
    FluidAudioDriver audio_driver;
    FluidMidiDriver midi_driver;
    std::unordered_map<std::string, int> soundfont_ids;

    std::vector<RecordedNoteEvent> events;
    uint64_t loop_length_ms{};

    std::jthread looper_thread;
    std::mutex playback_mutex;
    std::condition_variable playback_changed;
    std::optional<PlaybackTake> requested_take;
    uint64_t playback_generation{};

    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_changed;

    explicit Impl(const ApplicationConfig& config) {
        initializeFluidSynth(config);
        events.reserve(max_recorded_events);
    }

    void initializeFluidSynth(const ApplicationConfig& config) {
        settings.reset(new_fluid_settings());
        if (!settings) {
            throw std::runtime_error("Could not create FluidSynth settings");
        }

        fluid_settings_setint(settings.get(), "synth.threadsafe-api", 1);
        fluid_settings_setnum(settings.get(), "synth.gain", 0.5);
        fluid_settings_setstr(settings.get(), "midi.portname", "cpp-midi-looper");
        fluid_settings_setint(settings.get(), "midi.autoconnect", 1);
        fluid_settings_setint(settings.get(), "midi.realtime-prio", 50);

        synth.reset(new_fluid_synth(settings.get()));
        if (!synth) {
            throw std::runtime_error("Could not create FluidSynth synth");
        }

        std::unordered_map<std::string, int> loaded_files;
        for (const auto& definition : config.soundfonts) {
            const auto path = definition.file.string();
            auto [loaded, inserted] = loaded_files.try_emplace(path, -1);
            if (inserted) {
                loaded->second = loadSoundFont(path);
            }
            soundfont_ids.emplace(definition.id, loaded->second);
        }

        selectConfiguredSoundFont(config, config.loop_soundfont, loop_channel);
        selectConfiguredSoundFont(config, config.live_soundfont, live_channel);

        audio_driver.reset(new_fluid_audio_driver(settings.get(), synth.get()));
        if (!audio_driver) {
            throw std::runtime_error("Could not create FluidSynth audio driver");
        }

    }

    void start(Application& owner) {
        midi_driver.reset(new_fluid_midi_driver(
            settings.get(),
            &Application::midiCallback,
            &owner
        ));
        if (!midi_driver) {
            throw std::runtime_error("Could not create FluidSynth MIDI driver");
        }

        looper_thread = std::jthread([this](std::stop_token stop_token) {
            looperMain(stop_token);
        });
    }

    int loadSoundFont(const std::string& path) {
        const int soundfont_id = fluid_synth_sfload(synth.get(), path.c_str(), 0);
        if (soundfont_id == -1) {
            throw std::runtime_error("Could not load SoundFont: " + path);
        }
        return soundfont_id;
    }

    void selectSoundFont(
        const std::string& path,
        int channel,
        int soundfont_id,
        int bank,
        int preset
    ) {
        const int result = fluid_synth_program_select(
            synth.get(),
            channel,
            soundfont_id,
            bank,
            preset
        );

        #ifdef ZETA_MIDI_TRACE
        std::osyncstream{std::cerr}
            << "[program_select]"
            << " path=" << path
            << " sfid=" << soundfont_id
            << " channel=" << channel
            << " bank=" << bank
            << " preset=" << preset
            << " rc=" << result
            << "\n";
        #endif

        if (result != FLUID_OK) {
            throw std::runtime_error(
                "Could not select preset bank=" + std::to_string(bank) +
                " preset=" + std::to_string(preset) +
                " in SoundFont: " + path
            );
        }
    }

    void selectConfiguredSoundFont(
        const ApplicationConfig& config,
        std::string_view id,
        int channel
    ) {
        const auto& definition = config.soundfont(id);
        selectSoundFont(
            definition.file.string(),
            channel,
            soundfont_ids.at(definition.id),
            definition.bank,
            definition.preset
        );
    }

    void notifyLifecycleChanged() {
        lifecycle_changed.notify_all();
    }

    void stopLoopPlayback() {
        {
            std::lock_guard lock(playback_mutex);
            requested_take.reset();
            ++playback_generation;
        }
        playback_changed.notify_all();
    }

    void startLoopPlayback() {
        {
            std::lock_guard lock(playback_mutex);
            requested_take = PlaybackTake{
                .events = events,
                .length_ms = loop_length_ms,
            };
            ++playback_generation;
        }
        playback_changed.notify_all();
    }

    void stopPlaybackWorker() {
        if (!looper_thread.joinable()) {
            return;
        }

        looper_thread.request_stop();
        {
            std::lock_guard lock(playback_mutex);
            requested_take.reset();
            ++playback_generation;
        }
        playback_changed.notify_all();
        looper_thread.join();
    }

    void releaseResources() {
        midi_driver.reset();
        audio_driver.reset();
        synth.reset();
        settings.reset();
    }

    void looperMain(std::stop_token stop_token) {
        uint64_t observed_generation = 0;

        while (!stop_token.stop_requested()) {
            PlaybackTake take;
            uint64_t active_generation = 0;

            {
                std::unique_lock lock(playback_mutex);
                playback_changed.wait(lock, [&] {
                    return stop_token.stop_requested()
                        || playback_generation != observed_generation;
                });

                if (stop_token.stop_requested()) {
                    break;
                }

                observed_generation = playback_generation;
                if (!requested_take) {
                    continue;
                }

                take = *requested_take;
                active_generation = observed_generation;
            }

            auto loop_started_at = LooperClock::now();
            bool interrupted = false;

            while (!stop_token.stop_requested() && !interrupted) {
                for (const auto& event : take.events) {
                    const auto deadline = loop_started_at + Milliseconds(event.time_ms);
                    std::unique_lock lock(playback_mutex);
                    playback_changed.wait_until(lock, deadline, [&] {
                        return stop_token.stop_requested()
                            || playback_generation != active_generation;
                    });

                    if (stop_token.stop_requested()
                        || playback_generation != active_generation) {
                        interrupted = true;
                        break;
                    }

                    playRecordedEvent(event);
                }

                if (interrupted) {
                    break;
                }

                const auto loop_end = loop_started_at + Milliseconds(take.length_ms);
                std::unique_lock lock(playback_mutex);
                playback_changed.wait_until(lock, loop_end, [&] {
                    return stop_token.stop_requested()
                        || playback_generation != active_generation;
                });

                if (stop_token.stop_requested()
                    || playback_generation != active_generation) {
                    interrupted = true;
                    break;
                }

                loop_started_at += Milliseconds(take.length_ms);
            }

            allNotesOff();
        }
    }

    void playRecordedEvent(const RecordedNoteEvent& event) {
        #ifdef ZETA_MIDI_TRACE
        std::osyncstream{std::cerr}
            << "[loop playback]"
            << " channel=" << loop_channel
            << " kind=" << (
                event.kind == RecordedNoteKind::NoteOn ? "note_on" : "note_off"
            )
            << " key=" << event.key
            << " velocity=" << event.velocity
            << "\n";
        #endif

        if (event.kind == RecordedNoteKind::NoteOn) {
            fluid_synth_noteon(synth.get(), loop_channel, event.key, event.velocity);
        } else {
            fluid_synth_noteoff(synth.get(), loop_channel, event.key);
        }
    }

    void allNotesOff() {
        if (!synth) {
            return;
        }

        for (int channel = 0; channel < 16; ++channel) {
            fluid_synth_cc(synth.get(), channel, 64, 0);
            fluid_synth_cc(synth.get(), channel, 123, 0);
        }
    }
};

Application::Application(ApplicationConfig config)
    : config_(std::move(config)),
      impl_(std::make_unique<Impl>(config_)),
      states_(*this),
      fsm_(states_) {
    impl_->start(*this);
}

Application::~Application() {
    shutdownRequested();
    impl_->releaseResources();
}

void Application::run() {
    std::cout << "\nMIDI looper ready.\n";
    std::cout << "Play your controller: it should sound live.\n\n";
    std::cout << "Use the configured MIDI recording control to start.\n";

    std::unique_lock lock(impl_->lifecycle_mutex);
    impl_->lifecycle_changed.wait(lock, [this] {
        return !fsm_.shouldRun();
    });
}

void Application::shutdownRequested() {
    fsm_.shutdownRequested();
    impl_->notifyLifecycleChanged();
}

int Application::midiCallback(void* data, fluid_midi_event_t* event) noexcept {
    auto* application = static_cast<Application*>(data);
    try {
        return application->handleMidiEvent(event);
    } catch (...) {
        return FLUID_FAILED;
    }
}

int Application::handleMidiEvent(fluid_midi_event_t* event) {
    const int raw_type = fluid_midi_event_get_type(event);
    const auto type = classifyMidiMessage(raw_type);

    MidiMessage message{
        .native_event = event,
        .raw_type = raw_type,
        .channel = fluid_midi_event_get_channel(event),
        .key = fluid_midi_event_get_key(event),
        .velocity = fluid_midi_event_get_velocity(event),
        .control = fluid_midi_event_get_control(event),
        .value = fluid_midi_event_get_value(event),
        .program = fluid_midi_event_get_program(event),
        .pitch = fluid_midi_event_get_pitch(event),
    };

    if (std::ranges::any_of(config_.recording_controls, [&](const auto& control) {
        return control.matches(type, message);
    })) {
        const auto state = fsm_.primaryControlPressed(LooperClock::now());
        if (isTerminal(state)) {
            impl_->notifyLifecycleChanged();
        }
        return FLUID_OK;
    }

    if (type == MidiMessageType::ControlChange
        && message.control == expression_controller
        && message.value == 0) {
        std::cerr << "[midi ignored] CC 11 expression value 0\n";
        return FLUID_OK;
    }

    return fsm_.midiMessage(type, message, LooperClock::now());
}

int Application::monitorMidi(MidiMessage& message, MidiRoute route) {
    auto* event = static_cast<fluid_midi_event_t*>(message.native_event);
    #ifdef ZETA_MIDI_TRACE
    const int input_channel = message.channel;
    #endif
    const int output_channel = route == MidiRoute::LoopChannel
        ? loop_channel
        : live_channel;

    fluid_midi_event_set_channel(event, output_channel);
    message.channel = output_channel;

    const int result = fluid_synth_handle_midi_event(impl_->synth.get(), event);

    #ifdef ZETA_MIDI_TRACE
    std::osyncstream log{std::cerr};
    log
        << "[midi monitor]"
        << " route=" << (route == MidiRoute::LoopChannel ? "loop" : "live")
        << " input_channel=" << input_channel
        << " output_channel=" << output_channel
        << " type=0x" << std::hex << message.raw_type << std::dec;

    switch (classifyMidiMessage(message.raw_type)) {
    case MidiMessageType::NoteOn:
    case MidiMessageType::NoteOff:
        log
            << " key=" << message.key
            << " velocity=" << message.velocity;
        break;
    case MidiMessageType::ControlChange:
        log
            << " control=" << message.control
            << " value=" << message.value;
        break;
    case MidiMessageType::ProgramChange:
        log << " program=" << message.program;
        break;
    case MidiMessageType::PitchBend:
        log << " pitch=" << message.pitch;
        break;
    case MidiMessageType::Other:
        break;
    }

    log << " result=" << result << "\n";
    #endif

    return result;
}

void Application::stopLoopPlayback() {
    impl_->stopLoopPlayback();
}

void Application::silenceAllChannels() {
    impl_->allNotesOff();
}

void Application::resetTake() {
    impl_->events.clear();
    impl_->loop_length_ms = 0;
}

void Application::recordNote(
    RecordedNoteKind kind,
    const MidiMessage& message,
    Milliseconds offset
) {
    if (impl_->events.size() >= max_recorded_events) {
        return;
    }

    impl_->events.push_back(Impl::RecordedNoteEvent{
        .time_ms = static_cast<uint64_t>(offset.count()),
        .channel = message.channel,
        .key = message.key,
        .velocity = message.velocity,
        .kind = kind,
    });
}

void Application::commitTake(Milliseconds duration) {
    impl_->loop_length_ms = static_cast<uint64_t>(duration.count());
}

void Application::startLoopPlayback() {
    impl_->selectConfiguredSoundFont(config_, config_.live_soundfont, live_channel);
    impl_->startLoopPlayback();
}

void Application::showRecordingArmed() {
    std::cout
        << "Recording... use the configured MIDI control to stop and start looping.\n";
}

void Application::showLooping() {
    std::cout << "Looping. You can still play live over the loop.\n";
    std::cout << "Use the configured MIDI control to stop and quit.\n";
}

void Application::showNoTake() {
    std::cout << "No notes were recorded. Exiting.\n";
}

void Application::stopPlaybackWorker() {
    impl_->stopPlaybackWorker();
}

} // namespace zeta
