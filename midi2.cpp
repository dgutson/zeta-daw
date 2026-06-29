/*
midi_looper.cpp

Simple FluidSynth MIDI looper.
- Live MIDI input is passed through to FluidSynth.
- ENTER starts recording.
- ENTER again stops recording and starts looping.
- ENTER again exits.

Build:
  g++ -std=c++20 -Wall -Wextra -pedantic midi_looper.cpp \
      $(pkg-config --cflags --libs fluidsynth) \
      -pthread -o midi_looper

Run:
  ./midi_looper /path/to/soundfont.sf2
*/

#include <fluidsynth.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

#if 0
// for later
enum class MessageType : int {
    NoteOff       = 0x80,
    NoteOn        = 0x90,
    ControlChange = 0xB0,
    ProgramChange = 0xC0,
    PitchBend     = 0xE0,
};

enum class Controller : int {
    BankSelectMsb       = 0,
    ModulationWheel     = 1,
    Volume              = 7,
    Expression          = 11,
    BankSelectLsb       = 32,
    SustainPedal        = 64,
    AllSoundOff         = 120,
    ResetAllControllers = 121,
    AllNotesOff         = 123,
};
#endif

constexpr int MIDI_NOTE_OFF = 0x80;
constexpr int MIDI_NOTE_ON  = 0x90;
constexpr int MIDI_CC             = 0xB0;
constexpr int MIDI_EXPRESSION = 11;
constexpr int MIDI_PROGRAM_CHANGE = 0xC0;
constexpr int MIDI_PITCH_BEND     = 0xE0;
// Add inside Application private section:

static constexpr int live_channel_ = 0; // optional; mostly documentary
static constexpr int loop_channel_ = 1; // MIDI channel 2, zero-based API

struct SettingsDeleter {
    void operator()(fluid_settings_t* p) const noexcept {
        if (p) delete_fluid_settings(p);
    }
};

struct SynthDeleter {
    void operator()(fluid_synth_t* p) const noexcept {
        if (p) delete_fluid_synth(p);
    }
};

struct AudioDriverDeleter {
    void operator()(fluid_audio_driver_t* p) const noexcept {
        if (p) delete_fluid_audio_driver(p);
    }
};

struct MidiDriverDeleter {
    void operator()(fluid_midi_driver_t* p) const noexcept {
        if (p) delete_fluid_midi_driver(p);
    }
};

using FluidSettings    = std::unique_ptr<fluid_settings_t, SettingsDeleter>;
using FluidSynth       = std::unique_ptr<fluid_synth_t, SynthDeleter>;
using FluidAudioDriver = std::unique_ptr<fluid_audio_driver_t, AudioDriverDeleter>;
using FluidMidiDriver  = std::unique_ptr<fluid_midi_driver_t, MidiDriverDeleter>;

class Application {
public:
    explicit Application(std::string loop_soundfont_path, std::string live_soundfont_path)
    {
        initializeFluidSynth(loop_soundfont_path, live_soundfont_path);

        // Avoid allocations inside the MIDI callback for normal use.
        // Still a prototype: for serious live use, replace this with a lock-free ring buffer.
        events_.reserve(max_recorded_events_);

        looper_thread_ = std::jthread([this](std::stop_token stop_token) {
            looperMain(stop_token);
        });
    }

    ~Application() {
        shutdown();
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void runInteractive() {
        std::cout << "\nMIDI looper ready.\n";
        std::cout << "Play your controller: it should sound live.\n\n";

        std::cout << "Press ENTER to start recording.\n";
        std::cin.get();

        startRecording();

        std::cout << "Recording... press ENTER to stop recording and start looping.\n";
        std::cin.get();

        if (!stopRecordingAndStartLoop()) {
            std::cout << "No notes were recorded. Exiting.\n";
            return;
        }

        std::cout << "Looping. You can still play live over the loop.\n";
        std::cout << "Press ENTER to stop and quit.\n";
        std::cin.get();

        stopLoop();
    }

private:
    enum class Mode {
        Idle,
        WaitingFirstNote,
        Recording,
        Looping
    };

    struct RecordedNoteEvent {
        uint64_t time_ms{};
        int channel{};
        int key{};
        int velocity{};
        bool note_on{};
    };

    static constexpr std::size_t max_recorded_events_ = 16384;

    //std::string soundfont_path_;

    FluidSettings settings_;
    FluidSynth synth_;
    FluidAudioDriver audio_driver_;
    FluidMidiDriver midi_driver_;

    std::jthread looper_thread_;

    std::mutex mutex_;
    std::condition_variable cv_;

    std::vector<RecordedNoteEvent> events_;
    Clock::time_point recording_started_at_{};
    uint64_t loop_length_ms_{};

    std::atomic<Mode> mode_{Mode::Idle};
    std::atomic<bool> running_{true};

    void initializeFluidSynth(std::string loop_soundfont_path, std::string live_soundfont_path)
    {
        settings_.reset(new_fluid_settings());
        if (!settings_) {
            throw std::runtime_error("Could not create FluidSynth settings");
        }

        // Keep the public synth API protected because both the MIDI callback
        // and the looper thread can call into FluidSynth.
        fluid_settings_setint(settings_.get(), "synth.threadsafe-api", 1);

        // Keep gain moderate to avoid clipping with layered live + looped notes.
        fluid_settings_setnum(settings_.get(), "synth.gain", 0.5);

        // Easier to recognize in ALSA/CoreMIDI/JACK tools.
        fluid_settings_setstr(settings_.get(), "midi.portname", "cpp-midi-looper");

        // Supported by alsa_seq, coremidi, and jack according to current docs.
        fluid_settings_setint(settings_.get(), "midi.autoconnect", 1);

        // Linux MIDI drivers can use this setting. If the OS/user permissions
        // do not allow realtime scheduling, FluidSynth may fall back internally.
        fluid_settings_setint(settings_.get(), "midi.realtime-prio", 50);

        synth_.reset(new_fluid_synth(settings_.get()));
        if (!synth_) {
            throw std::runtime_error("Could not create FluidSynth synth");
        }

        loadSoundFont(loop_soundfont_path, loop_channel_, 0, 34);
        loadSoundFont(live_soundfont_path, live_channel_, 0, 0);

        audio_driver_.reset(new_fluid_audio_driver(settings_.get(), synth_.get()));
        if (!audio_driver_) {
            throw std::runtime_error("Could not create FluidSynth audio driver");
        }

        midi_driver_.reset(
            new_fluid_midi_driver(settings_.get(), &Application::midiCallback, this)
        );

        if (!midi_driver_) {
            throw std::runtime_error("Could not create FluidSynth MIDI driver");
        }
    }

    void loadSoundFont(std::string path, int channel, int bank, int preset)
    {
        const int soundfont_id =
            fluid_synth_sfload(synth_.get(), path.c_str(), 0);

        if (soundfont_id == -1) {
            throw std::runtime_error("Could not load SoundFont: " + path);
        }

        // Channel 0, loaded soundfont, bank 0, preset 0.
        const auto rc = fluid_synth_program_select(synth_.get(), channel, soundfont_id, bank, preset);

        #ifdef DEBUG
        std::cerr
            << "[program_select]"
            << " path=" << path
            << " sfid=" << soundfont_id
            << " channel=" << channel
            << " bank=0"
            << " preset=" << (channel == loop_channel_ ? 34 : 0)
            << " rc=" << rc
            << "\n";
        #endif

        if (rc != FLUID_OK) {
            throw std::runtime_error(
                "Could not select preset bank=" + std::to_string(bank) +
                " preset=" + std::to_string(preset) +
                " in SoundFont: " + path
            );
        }
    }

    static int midiCallback(void* data, fluid_midi_event_t* event) noexcept {
        auto* app = static_cast<Application*>(data);

        try {
            return app->handleMidiEvent(event);
        } catch (...) {
            return FLUID_FAILED;
        }
    }

    int handleMidiEvent(fluid_midi_event_t* event) {
        const int type = fluid_midi_event_get_type(event);
        const int velocity = fluid_midi_event_get_velocity(event);

        if (type == MIDI_CC &&
            fluid_midi_event_get_control(event) == MIDI_EXPRESSION &&
            fluid_midi_event_get_value(event) == 0) {

            std::cerr << "[midi ignored] CC 11 expression value 0\n";
            return FLUID_OK;
        }

        const Mode mode = mode_.load(std::memory_order_relaxed);

        const bool waiting_first_note = mode == Mode::WaitingFirstNote;
        const bool recording = mode == Mode::Recording;
        const bool armed_or_recording = waiting_first_note || recording;

        #ifdef DEBUG
        const auto channel = fluid_midi_event_get_channel(event);
        if (type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF) {
            std::cerr
                << "[midi]"
                << " mode=" << (recording ? "recording" : "live")
                << " type=0x" << std::hex << type << std::dec
                << " channel=" << channel
                << " key=" << fluid_midi_event_get_key(event)
                << " velocity=" << velocity
                << "\n";
        } else if (type == MIDI_CC) {
           const int cc = fluid_midi_event_get_control(event);
           const int value = fluid_midi_event_get_value(event);

           std::cerr
               << "[midi cc]"
               << " ch=" << channel
               << " cc=" << cc
               << " value=" << value;

           if (cc == 0)   std::cerr << "  BANK_SELECT_MSB";
           if (cc == 32)  std::cerr << "  BANK_SELECT_LSB";
           if (cc == 7)   std::cerr << "  VOLUME";
           if (cc == 11)  std::cerr << "  EXPRESSION";
           if (cc == 120) std::cerr << "  ALL_SOUND_OFF";
           if (cc == 121) std::cerr << "  RESET_ALL_CONTROLLERS";
           if (cc == 123) std::cerr << "  ALL_NOTES_OFF";

           std::cerr << "\n";
       } else if (type == MIDI_PROGRAM_CHANGE) {
           std::cerr
               << "[midi program]"
               << " ch=" << channel
               << " program=" << fluid_midi_event_get_program(event)
               << "\n";
       } else if (type == MIDI_PITCH_BEND) {
           std::cerr
               << "[midi pitch]"
               << " ch=" << channel
               << " pitch=" << fluid_midi_event_get_pitch(event)
               << "\n";
       } else {
           std::cerr
               << "[midi other]"
               << " type=0x" << std::hex << type << std::dec
               << " ch=" << channel
               << "\n";
       }
        #endif

        if (armed_or_recording) {
            fluid_midi_event_set_channel(event, loop_channel_);
        }

        // Always play live input.
        const int result = fluid_synth_handle_midi_event(synth_.get(), event);

        #ifdef DEBUG
        {
            int sfid = -1;
            int bank = -1;
            int preset = -1;

            fluid_synth_get_program(
                synth_.get(),
                live_channel_,
                &sfid,
                &bank,
                &preset
            );

            std::cerr
                << "[live program after]"
                << " sfid=" << sfid
                << " bank=" << bank
                << " preset=" << preset
                << " result=" << result
                << "\n";
        }
        #endif

        if (type != MIDI_NOTE_ON && type != MIDI_NOTE_OFF) {
            return result;
        }


        const bool note_on =
            type == MIDI_NOTE_ON && velocity > 0;

        const bool note_off =
            type == MIDI_NOTE_OFF || (type == MIDI_NOTE_ON && velocity == 0);

        if (!note_on && !note_off) {
            return result;
        }

        if (!armed_or_recording) {
            return result;
        }

        const auto now = Clock::now();

        std::lock_guard lock(mutex_);

        if (events_.size() >= max_recorded_events_) {
            return result;
        }

        if (waiting_first_note) {
            if (!note_on) {
                return result;
            }

            recording_started_at_ = now;
            mode_.store(Mode::Recording);
        }

        events_.push_back(RecordedNoteEvent{
            .time_ms = static_cast<uint64_t>(
                   std::chrono::duration_cast<Ms>(now - recording_started_at_).count()
               ),
            .channel = fluid_midi_event_get_channel(event),
            .key = fluid_midi_event_get_key(event),
            .velocity = velocity,
            .note_on = note_on
        });

        return result;
    }

    void startRecording() {
        stopLoop();
        allNotesOff();

        {
            std::lock_guard lock(mutex_);

            events_.clear();
            loop_length_ms_ = 0;
            //recording_started_at_ = Clock::now();
        }

        mode_.store(Mode::WaitingFirstNote);
    }

    bool stopRecordingAndStartLoop() {
        const auto stopped_at = Clock::now();

        {
            std::lock_guard lock(mutex_);

            const Mode mode = mode_.load();

            if (mode == Mode::WaitingFirstNote) {
                mode_.store(Mode::Idle);
                return false;
            }

            if (mode != Mode::Recording) {
                return false;
            }

            loop_length_ms_ = static_cast<uint64_t>(
                std::chrono::duration_cast<Ms>(
                    stopped_at - recording_started_at_
                ).count()
            );

            if (events_.empty() || loop_length_ms_ == 0) {
                mode_.store(Mode::Idle);
                return false;
            }

            mode_.store(Mode::Looping);
        }

        cv_.notify_all();
        return true;
    }

    void stopLoop() {
        mode_.store(Mode::Idle);
        cv_.notify_all();
        allNotesOff();
    }

    void looperMain(std::stop_token stop_token) {
        while (!stop_token.stop_requested() && running_.load()) {
            std::vector<RecordedNoteEvent> snapshot;
            uint64_t loop_length_ms = 0;

            {
                std::unique_lock lock(mutex_);

                cv_.wait(lock, [&] {
                    return stop_token.stop_requested()
                        || !running_.load()
                        || mode_.load() == Mode::Looping;
                });

                if (stop_token.stop_requested() || !running_.load()) {
                    break;
                }

                snapshot = events_;
                loop_length_ms = loop_length_ms_;
            }

            if (snapshot.empty() || loop_length_ms == 0) {
                mode_.store(Mode::Idle);
                continue;
            }

            auto loop_started_at = Clock::now();

            while (
                !stop_token.stop_requested()
                && running_.load()
                && mode_.load() == Mode::Looping
            ) {
                bool interrupted = false;

                for (const auto& event : snapshot) {
                    const auto event_time = loop_started_at + Ms(event.time_ms);

                    if (!waitUntilOrStopped(stop_token, event_time)) {
                        interrupted = true;
                        break;
                    }

                    playRecordedEvent(event);
                }

                if (interrupted) {
                    break;
                }

                const auto loop_end = loop_started_at + Ms(loop_length_ms);
                if (!waitUntilOrStopped(stop_token, loop_end)) {
                    break;
                }

                loop_started_at += Ms(loop_length_ms);
            }

            allNotesOff();
        }
    }

    bool waitUntilOrStopped(
        std::stop_token stop_token,
        Clock::time_point deadline
    ) {
        std::unique_lock lock(mutex_);

        cv_.wait_until(lock, deadline, [&] {
            return stop_token.stop_requested()
                || !running_.load()
                || mode_.load() != Mode::Looping;
        });

        return !stop_token.stop_requested()
            && running_.load()
            && mode_.load() == Mode::Looping;
    }

    void playRecordedEvent(const RecordedNoteEvent& event) {
        if (event.note_on) {
            fluid_synth_noteon(
                synth_.get(),
                loop_channel_,
                event.key,
                event.velocity
            );
        } else {
            fluid_synth_noteoff(
                synth_.get(),
                loop_channel_,
                event.key
            );
        }
    }

    void allNotesOff() {
        if (!synth_) {
            return;
        }

        for (int channel = 0; channel < 16; ++channel) {
            // CC 64 = sustain pedal off.
            fluid_synth_cc(synth_.get(), channel, 64, 0);

            // CC 123 = all notes off.
            fluid_synth_cc(synth_.get(), channel, 123, 0);
        }
    }

    static uint64_t millisSince(Clock::time_point start) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<Ms>(Clock::now() - start).count()
        );
    }

    void shutdown() {
        if (!running_.exchange(false)) {
            return;
        }

        mode_.store(Mode::Idle);
        cv_.notify_all();

        if (looper_thread_.joinable()) {
            looper_thread_.request_stop();
            cv_.notify_all();
            looper_thread_.join();
        }

        allNotesOff();

        // Important order: stop external callbacks before destroying synth.
        midi_driver_.reset();
        audio_driver_.reset();
        synth_.reset();
        settings_.reset();
    }
};

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " /path/to/loop_soundfont.sf2  /path/to/live_soundfont.sf2\n";
        return 1;
    }

    try {
        Application app{argv[1], argv[2]};
        app.runInteractive();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }
}
