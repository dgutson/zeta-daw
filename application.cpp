#include "application.hpp"
#include "synth_engine.hpp"

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
#include <utility>
#include <vector>

namespace zeta {
namespace {

constexpr int live_channel = 0;
constexpr int loop_channel = 1;
constexpr int expression_controller = 11;
constexpr std::size_t max_recorded_events = 16384;

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

    SynthEngine synth_engine;

    std::vector<RecordedNoteEvent> events;
    uint64_t loop_length_ms{};

    std::jthread looper_thread;
    std::mutex playback_mutex;
    std::condition_variable playback_changed;
    std::optional<PlaybackTake> requested_take;
    uint64_t playback_generation{};

    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_changed;

    explicit Impl(const ApplicationConfig& config) : synth_engine(config) {
        synth_engine.select(config.soundfont(config.loop_soundfont), loop_channel);
        synth_engine.select(config.soundfont(config.live_soundfont), live_channel);
        events.reserve(max_recorded_events);
    }

    void startPlaybackWorker() {
        looper_thread = std::jthread([this](std::stop_token stop_token) {
            looperMain(stop_token);
        });
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
            synth_engine.noteOn(loop_channel, event.key, event.velocity);
        } else {
            synth_engine.noteOff(loop_channel, event.key);
        }
    }

    void allNotesOff() {
        synth_engine.allNotesOff();
    }
};

Application::Application(ApplicationConfig config)
    : Application(std::move(config), makeMidiInput()) {}

Application::Application(
    ApplicationConfig config,
    std::unique_ptr<MidiInput> midi_input
)
    : config_(std::move(config)),
      impl_(std::make_unique<Impl>(config_)),
      states_(*this),
      fsm_(states_),
      midi_input_(std::move(midi_input)) {
    if (!midi_input_) {
        throw std::invalid_argument("MIDI input must not be null");
    }

    midi_input_->start([this](MidiEvent event) {
        handleMidiEvent(std::move(event));
    });
    impl_->startPlaybackWorker();
}

Application::~Application() {
    midi_input_->stop();
    shutdownRequested();
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

void Application::handleMidiEvent(MidiEvent event) noexcept {
    try {
        const auto type = event.type;
        const auto& message = event.message;

        if (std::ranges::any_of(config_.recording_controls, [&](const auto& control) {
            return control.matches(type, message);
        })) {
            const auto state = fsm_.primaryControlPressed(LooperClock::now());
            if (isTerminal(state)) {
                impl_->notifyLifecycleChanged();
            }
            return;
        }

        if (type == MidiMessageType::ControlChange
            && message.control == expression_controller
            && message.value == 0) {
            std::cerr << "[midi ignored] CC 11 expression value 0\n";
            return;
        }

        fsm_.midiMessage(type, message, LooperClock::now());
    } catch (const std::exception& error) {
        std::cerr << "[MIDI handling error] " << error.what() << '\n';
    } catch (...) {
        std::cerr << "[MIDI handling error] unknown failure\n";
    }
}

int Application::monitorMidi(const MidiMessage& message, MidiRoute route) {
    const int output_channel = route == MidiRoute::LoopChannel
        ? loop_channel
        : live_channel;

    const int result = impl_->synth_engine.send(message, output_channel);

    #ifdef ZETA_MIDI_TRACE
    const auto type = classifyMidiMessage(message.raw_type);
    std::osyncstream log{std::cerr};
    log
        << "[midi monitor]"
        << " route=" << (route == MidiRoute::LoopChannel ? "loop" : "live")
        << " input_channel=" << message.channel
        << " output_channel=" << output_channel
        << " type=0x" << std::hex << message.raw_type << std::dec;

    switch (type) {
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
    case MidiMessageType::PolyphonicKeyPressure:
        log << " key=" << message.key << " pressure=" << message.pressure;
        break;
    case MidiMessageType::ChannelPressure:
        log << " pressure=" << message.pressure;
        break;
    case MidiMessageType::MachineControl:
        log << " mmc_command=" << message.machine_control_command;
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
    impl_->synth_engine.select(
        config_.soundfont(config_.live_soundfont),
        live_channel
    );
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
