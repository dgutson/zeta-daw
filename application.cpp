#include "application.hpp"

#include "loop_slot_group.hpp"
#include "octave_transposer.hpp"
#include "soundfont_selector.hpp"
#include "synth_engine.hpp"

#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <syncstream>
#include <utility>

namespace zeta {
namespace {

constexpr int live_channel = 0;
constexpr int expression_controller = 11;
} // namespace

struct Application::Impl {
    SynthEngine synth_engine;
    SoundFontSelector soundfont_selector;
    OctaveTransposer live_transposer;
    LoopSlotGroup loop_slots;

    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_changed;

    explicit Impl(const ApplicationConfig& application_config)
        : synth_engine(application_config),
          soundfont_selector(application_config.soundfonts),
          loop_slots(application_config.loop_slots, synth_engine) {
        selectCurrentLiveSoundFont();
    }

    void selectCurrentLiveSoundFont() {
        synth_engine.select(soundfont_selector.current(), live_channel);
    }

    void selectNextLiveSoundFont() {
        synth_engine.select(soundfont_selector.next(), live_channel);
        reportCurrentSoundFont();
    }

    void selectNextLoopSlotSoundFont(SlotId id) {
        loop_slots.selectSoundFont(id, soundfont_selector.next());
        reportCurrentSoundFont();
    }

    void selectLiveSoundFontByNote(int key) {
        const auto* soundfont = selectSoundFontByNote(key);
        if (soundfont) {
            synth_engine.select(*soundfont, live_channel);
        }
    }

    const SoundFontDefinition* selectSoundFontByNote(int key) {
        const auto* soundfont = soundfont_selector.selectByKey(key);
        if (!soundfont) {
            std::cerr << "No SoundFont mapped for MIDI key " << key << '\n';
            return nullptr;
        }
        reportCurrentSoundFont();
        return soundfont;
    }

    void reportCurrentSoundFont() const {
        std::cout << "SoundFont selected: "
                  << soundfont_selector.current().id << '\n';
    }

    void notifyLifecycleChanged() {
        lifecycle_changed.notify_all();
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
    impl_->synth_engine.allNotesOff();
    midi_ready_.store(true, std::memory_order_release);
}

Application::~Application() {
    midi_ready_.store(false, std::memory_order_release);
    midi_input_->stop();
    shutdownRequested();
}

void Application::run() {
    std::cout << "\nMIDI looper ready.\n";
    std::cout << "Play your controller: it should sound live.\n\n";
    if (config_.next_soundfont_control) {
        std::cout << "Use the configured Next control to select a SoundFont.\n";
    }
    if (config_.soundfont_by_note_control) {
        std::cout
            << "Use the configured SoundFont-by-note control, then a keyed note, "
            << "to select a SoundFont.\n";
    }
    std::cout
        << "Use the loop-slot control, then a configured slot note, to arm or "
        << "stop a loop slot.\n";

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

        if (config_.loop_slot_by_note_control.matches(type, message)) {
            fsm_.loopSlotControlPressed(LooperClock::now());
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

int Application::monitorLiveMidi(const MidiMessage& message) {
    const auto transposed = impl_->live_transposer.transpose(message);
    const int result = impl_->synth_engine.send(transposed, live_channel);

    #ifdef ZETA_MIDI_TRACE
    std::osyncstream{std::cerr}
        << "[midi monitor] route=live"
        << " input_channel=" << message.channel
        << " output_channel=" << live_channel
        << " type=0x" << std::hex << message.raw_type << std::dec
        << " result=" << result << "\n";
    #endif

    return result;
}

int Application::monitorLoopSlotMidi(
    SlotId slot,
    const MidiMessage& message
) {
    const int result = impl_->loop_slots.monitorMidi(slot, message);

    #ifdef ZETA_MIDI_TRACE
    std::osyncstream{std::cerr}
        << "[midi monitor] route=loop_slot"
        << " slot=" << slot
        << " input_channel=" << message.channel
        << " output_channel=" << impl_->loop_slots.channel(slot)
        << " type=0x" << std::hex << message.raw_type << std::dec
        << " result=" << result << "\n";
    #endif

    return result;
}

LoopSlotSelectionResult Application::requestLoopSlotSelection(int key) {
    return impl_->loop_slots.requestSelection(
        key,
        impl_->soundfont_selector.current(),
        impl_->live_transposer
    );
}

void Application::cancelLoopSlotRecording(SlotId slot) {
    impl_->loop_slots.cancelRecording(slot);
}

void Application::completeLoopSlotRecording(
    SlotId slot,
    const TakeTiming& timing
) {
    impl_->loop_slots.completeRecording(slot, timing);
}

void Application::terminateLoopSlots() {
    impl_->loop_slots.terminateAll();
}

void Application::selectCurrentLiveSoundFont() {
    impl_->selectCurrentLiveSoundFont();
}

void Application::selectNextLiveSoundFont() {
    impl_->selectNextLiveSoundFont();
}

void Application::selectNextLoopSlotSoundFont(SlotId slot) {
    impl_->selectNextLoopSlotSoundFont(slot);
}

void Application::selectLiveSoundFontByNote(int key) {
    impl_->selectLiveSoundFontByNote(key);
}

// Slot identity and raw MIDI key are distinct output-command domain values.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Application::selectLoopSlotSoundFontByNote(SlotId slot, int key) {
    const auto* soundfont = impl_->selectSoundFontByNote(key);
    if (soundfont) {
        impl_->loop_slots.selectSoundFont(slot, *soundfont);
    }
}

void Application::octaveDownLive() {
    impl_->live_transposer.octaveDown();
}

void Application::octaveUpLive() {
    impl_->live_transposer.octaveUp();
}

void Application::octaveDownLoopSlot(SlotId slot) {
    impl_->loop_slots.octaveDown(slot);
}

void Application::octaveUpLoopSlot(SlotId slot) {
    impl_->loop_slots.octaveUp(slot);
}

void Application::recordNote(
    SlotId slot,
    RecordedNoteKind kind,
    const MidiMessage& message,
    Milliseconds offset
) {
    impl_->loop_slots.recordNote(slot, kind, message, offset);
}

void Application::showRecordingArmed(SlotId slot) {
    std::cout << "Loop slot " << slot + 1
              << " armed. Play a note to start recording.\n";
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

void Application::showLooping(SlotId slot) {
    std::cout << "Loop slot " << slot + 1
              << " looping. You can still play live over every loop.\n";
}

void Application::showNoTake(SlotId slot) {
    std::cout << "Loop slot " << slot + 1
              << " pending take canceled. No take can resume.\n";
}

void Application::silenceAllChannels() {
    impl_->synth_engine.allNotesOff();
}

} // namespace zeta
