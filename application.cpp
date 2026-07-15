#include "application.hpp"
#include "octave_transposer.hpp"
#include "synth_engine.hpp"

#include <array>
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
constexpr std::size_t midi_channel_count = 16;
constexpr std::size_t midi_key_count = 128;
constexpr std::size_t max_recorded_events = 16384;

int channelFor(MidiRoute route) noexcept {
    return route == MidiRoute::LoopChannel ? loop_channel : live_channel;
}

std::size_t soundFontSelectionIndex(int channel, int key) noexcept {
    return static_cast<std::size_t>(channel) * midi_key_count
        + static_cast<std::size_t>(key);
}

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
        Milliseconds duration{};
    };

    SynthEngine synth_engine;
    const ApplicationConfig& config;
    std::size_t current_soundfont{};
    std::array<
        std::optional<std::size_t>,
        midi_channel_count * midi_key_count
    > soundfont_note_selections{};
    OctaveTransposer live_transposer;
    OctaveTransposer loop_transposer;

    std::vector<RecordedNoteEvent> events;
    Milliseconds loop_duration{};

    std::jthread looper_thread;
    std::mutex playback_mutex;
    std::condition_variable playback_changed;
    std::optional<PlaybackTake> requested_take;
    uint64_t playback_generation{};

    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_changed;

    explicit Impl(const ApplicationConfig& application_config)
        : synth_engine(application_config),
          config(application_config) {
        for (const auto& selection : config.soundfont_note_selections) {
            soundfont_note_selections.at(soundFontSelectionIndex(
                selection.channel,
                selection.key
            )) = selection.soundfont_index;
        }
        selectCurrentSoundFont(MidiRoute::LoopChannel);
        selectCurrentSoundFont(MidiRoute::LiveChannel);
        events.reserve(max_recorded_events);
    }

    void selectCurrentSoundFont(MidiRoute route) {
        synth_engine.select(
            config.soundfonts.at(current_soundfont),
            channelFor(route)
        );
    }

    void selectNextSoundFont(MidiRoute route) {
        current_soundfont = (current_soundfont + 1) % config.soundfonts.size();
        selectCurrentSoundFont(route);
        reportCurrentSoundFont();
    }

    void selectSoundFontByNote(MidiRoute route, int input_channel, int key) {
        const auto soundfont = soundfont_note_selections.at(
            soundFontSelectionIndex(input_channel, key)
        );
        if (!soundfont) {
            std::cerr
                << "No SoundFont mapped for MIDI channel "
                << input_channel + 1
                << " key " << key << '\n';
            return;
        }

        current_soundfont = *soundfont;
        selectCurrentSoundFont(route);
        reportCurrentSoundFont();
    }

    void reportCurrentSoundFont() const {
        std::cout << "SoundFont selected: "
                  << config.soundfonts[current_soundfont].id << '\n';
    }

    OctaveTransposer& transposerFor(MidiRoute route) noexcept {
        return route == MidiRoute::LoopChannel
            ? loop_transposer
            : live_transposer;
    }

    void startPlaybackWorker() {
        looper_thread = std::jthread([this](std::stop_token stop_token) {
            looperMain(std::move(stop_token));
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
                .duration = loop_duration,
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

            if (!isPlayableLoopDuration(take.duration)) {
                std::osyncstream{std::cerr}
                    << "[loop playback error] zero-length take ignored\n";
                continue;
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

                const auto loop_end = loop_started_at + take.duration;
                std::unique_lock lock(playback_mutex);
                playback_changed.wait_until(lock, loop_end, [&] {
                    return stop_token.stop_requested()
                        || playback_generation != active_generation;
                });

                if (stop_token.stop_requested()
                    || playback_generation != active_generation) {
                    break;
                }

                loop_started_at += take.duration;
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

    midi_input_->start(
        config_.midi_control_change_mappings,
        [this](MidiEvent event) {
            handleMidiEvent(event);
        }
    );
    impl_->allNotesOff();
    midi_ready_.store(true, std::memory_order_release);
    impl_->startPlaybackWorker();
}

Application::~Application() {
    midi_ready_.store(false, std::memory_order_release);
    midi_input_->stop();
    shutdownRequested();
}

bool Application::isPlayableLoopDuration(Milliseconds duration) noexcept {
    return duration > Milliseconds::zero();
}

void Application::run() {
    std::cout << "\nMIDI looper ready.\n";
    std::cout << "Play your controller: it should sound live.\n\n";
    if (config_.next_soundfont_control) {
        std::cout << "Use the configured Next control to select a SoundFont.\n";
    }
    if (config_.soundfont_by_note_control) {
        std::cout
            << "Use the configured SoundFont-by-note control, then a mapped note, "
            << "to select a SoundFont.\n";
    }
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
    if (!midi_ready_.load(std::memory_order_acquire)) {
        return;
    }

    try {
        const auto type = event.type;
        const auto& message = event.message;

        if (config_.recording_control.matches(type, message)) {
            fsm_.recordingControlPressed(LooperClock::now());
            return;
        }

        if (config_.next_soundfont_control
            && config_.next_soundfont_control->matches(type, message)) {
            fsm_.nextSoundFontPressed();
            return;
        }

        if (config_.soundfont_by_note_control
            && config_.soundfont_by_note_control->matches(type, message)) {
            fsm_.soundFontByNotePressed();
            return;
        }

        if (config_.octave_down_control.matches(type, message)) {
            fsm_.octaveDownPressed();
            return;
        }

        if (config_.octave_up_control.matches(type, message)) {
            fsm_.octaveUpPressed();
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
    const int output_channel = channelFor(route);
    const auto transposed = impl_->transposerFor(route).transpose(message);

    const int result = impl_->synth_engine.send(transposed, output_channel);

    #ifdef ZETA_MIDI_TRACE
    const auto type = classifyMidiMessage(transposed.raw_type);
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
            << " key=" << transposed.key
            << " velocity=" << transposed.velocity;
        break;
    case MidiMessageType::ControlChange:
        log
            << " control=" << transposed.control
            << " value=" << transposed.value;
        break;
    case MidiMessageType::ProgramChange:
        log << " program=" << transposed.program;
        break;
    case MidiMessageType::PitchBend:
        log << " pitch=" << transposed.pitch;
        break;
    case MidiMessageType::PolyphonicKeyPressure:
        log
            << " key=" << transposed.key
            << " pressure=" << transposed.pressure;
        break;
    case MidiMessageType::ChannelPressure:
        log << " pressure=" << transposed.pressure;
        break;
    case MidiMessageType::MachineControl:
        log << " mmc_command=" << transposed.machine_control_command;
        break;
    case MidiMessageType::Other:
        break;
    }

    log << " result=" << result << "\n";
    #endif

    return result;
}

void Application::selectCurrentSoundFont(MidiRoute route) {
    impl_->selectCurrentSoundFont(route);
}

void Application::selectNextSoundFont(MidiRoute route) {
    impl_->selectNextSoundFont(route);
}

void Application::selectSoundFontByNote(
    MidiRoute route,
    int input_channel,
    int key
) {
    impl_->selectSoundFontByNote(route, input_channel, key);
}

void Application::octaveDown(MidiRoute route) {
    impl_->transposerFor(route).octaveDown();
}

void Application::octaveUp(MidiRoute route) {
    impl_->transposerFor(route).octaveUp();
}

void Application::stopLoopPlayback() {
    impl_->stopLoopPlayback();
}

void Application::silenceAllChannels() {
    impl_->allNotesOff();
}

void Application::resetTake() {
    impl_->events.clear();
    impl_->loop_duration = Milliseconds::zero();
}

void Application::recordNote(
    RecordedNoteKind kind,
    const MidiMessage& message,
    Milliseconds offset
) {
    if (impl_->events.size() >= max_recorded_events) {
        return;
    }

    const auto transposed = impl_->loop_transposer.transpose(message);

    impl_->events.push_back(Impl::RecordedNoteEvent{
        .time_ms = static_cast<uint64_t>(offset.count()),
        .channel = message.channel,
        .key = transposed.key,
        .velocity = transposed.velocity,
        .kind = kind,
    });
}

void Application::commitTake(Milliseconds duration) {
    impl_->loop_duration = duration;
}

void Application::startLoopPlayback() {
    impl_->selectCurrentSoundFont(MidiRoute::LiveChannel);
    impl_->startLoopPlayback();
}

void Application::showRecordingArmed() {
    std::cout << "Recording... Play a note to start.\n";
    if (config_.next_soundfont_control) {
        std::cout
            << "Next can change the pending SoundFont before the first note.\n";
    }
    if (config_.soundfont_by_note_control) {
        std::cout
            << "SoundFont-by-note can change the pending SoundFont before the "
            << "first recording note.\n";
    }
}

void Application::showLooping() {
    std::cout << "Looping. You can still play live over the loop.\n";
    if (config_.next_soundfont_control) {
        std::cout << "Use Next to change the live SoundFont.\n";
    }
    if (config_.soundfont_by_note_control) {
        std::cout
            << "Use SoundFont-by-note followed by a mapped note to change the "
            << "live SoundFont.\n";
    }
    std::cout
        << "Use the configured MIDI control to stop the loop and return to Ready.\n";
}

void Application::showNoTake() {
    std::cout << "No notes were recorded. Ready for a new loop.\n";
}

void Application::stopPlaybackWorker() {
    impl_->stopPlaybackWorker();
}

} // namespace zeta
