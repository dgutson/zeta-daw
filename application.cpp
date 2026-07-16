#include "application.hpp"

#include "loop_slot.hpp"
#include "octave_transposer.hpp"
#include "soundfont_selector.hpp"
#include "synth_engine.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <syncstream>
#include <utility>
#include <vector>

namespace zeta {
namespace {

constexpr int live_channel = 0;
constexpr int expression_controller = 11;
constexpr std::size_t max_recorded_events = 16384;
constexpr std::size_t midi_key_count = 128;

} // namespace

struct Application::Impl {
    SynthEngine synth_engine;
    SoundFontSelector soundfont_selector;
    OctaveTransposer live_transposer;
    std::vector<std::unique_ptr<LoopSlot>> loop_slots;
    std::array<std::optional<SlotId>, midi_key_count> slots_by_key{};
    std::vector<RecordedLoopEvent> pending_events;

    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_changed;

    explicit Impl(const ApplicationConfig& application_config)
        : synth_engine(application_config),
          soundfont_selector(application_config.soundfonts) {
        loop_slots.reserve(application_config.loop_slots.size());
        for (SlotId id = 0; id < application_config.loop_slots.size(); ++id) {
            const auto& definition = application_config.loop_slots[id];
            auto slot = std::make_unique<LoopSlot>(
                id,
                definition,
                synth_engine
            );
            slots_by_key[static_cast<std::size_t>(slot->selectionKey())] =
                slot->id();
            loop_slots.push_back(std::move(slot));
        }

        pending_events.reserve(max_recorded_events);
        selectCurrentLiveSoundFont();
    }

    LoopSlot& slot(SlotId id) {
        return *loop_slots.at(id);
    }

    const LoopSlot& slot(SlotId id) const {
        return *loop_slots.at(id);
    }

    void selectCurrentLiveSoundFont() {
        synth_engine.select(soundfont_selector.current(), live_channel);
    }

    void selectNextLiveSoundFont() {
        synth_engine.select(soundfont_selector.next(), live_channel);
        reportCurrentSoundFont();
    }

    void selectNextLoopSlotSoundFont(SlotId id) {
        slot(id).selectSoundFont(soundfont_selector.next());
        reportCurrentSoundFont();
    }

    void selectLiveSoundFontByNote(int key) {
        const auto* soundfont = selectSoundFontByNote(key);
        if (soundfont) {
            synth_engine.select(*soundfont, live_channel);
        }
    }

    void selectLoopSlotSoundFontByNote(LoopSlot& target, int key) {
        const auto* soundfont = selectSoundFontByNote(key);
        if (soundfont) {
            target.selectSoundFont(*soundfont);
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
    const int result = impl_->slot(slot).monitorMidi(message);

    #ifdef ZETA_MIDI_TRACE
    std::osyncstream{std::cerr}
        << "[midi monitor] route=loop_slot"
        << " slot=" << slot
        << " input_channel=" << message.channel
        << " output_channel=" << impl_->slot(slot).channel()
        << " type=0x" << std::hex << message.raw_type << std::dec
        << " result=" << result << "\n";
    #endif

    return result;
}

std::optional<LoopSlotSelection> Application::loopSlotByKey(int key) const {
    const auto id = impl_->slots_by_key.at(static_cast<std::size_t>(key));
    if (!id) {
        std::cerr << "No loop slot mapped for MIDI key " << key << '\n';
        return std::nullopt;
    }

    const auto state = impl_->slot(id.value()).playbackState();
    if (state == LoopSlotPlaybackState::Terminated) {
        return std::nullopt;
    }
    return LoopSlotSelection{
        .id = id.value(),
        .state = state == LoopSlotPlaybackState::Looping
            ? SelectableLoopSlotState::Looping
            : SelectableLoopSlotState::Muted,
    };
}

void Application::prepareLoopSlot(SlotId slot) {
    impl_->slot(slot).prepareRecording(
        impl_->soundfont_selector.current(),
        impl_->live_transposer
    );
}

void Application::cancelLoopSlotRecording(SlotId slot) {
    impl_->slot(slot).cancelRecording();
}

void Application::muteLoopSlot(SlotId slot) {
    impl_->slot(slot).muteRequested();
    std::cout << "Loop slot " << slot + 1 << " muted.\n";
}

void Application::startLoopSlot(SlotId slot) {
    impl_->slot(slot).startRequested();
}

void Application::terminateLoopSlots() {
    for (auto& slot : impl_->loop_slots) {
        slot->terminationRequested();
    }
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

void Application::selectLoopSlotSoundFontByNote(SlotId slot, int key) {
    impl_->selectLoopSlotSoundFontByNote(impl_->slot(slot), key);
}

void Application::octaveDownLive() {
    impl_->live_transposer.octaveDown();
}

void Application::octaveUpLive() {
    impl_->live_transposer.octaveUp();
}

void Application::octaveDownLoopSlot(SlotId slot) {
    impl_->slot(slot).octaveDown();
}

void Application::octaveUpLoopSlot(SlotId slot) {
    impl_->slot(slot).octaveUp();
}

void Application::resetPendingTake() {
    impl_->pending_events.clear();
}

void Application::recordNote(
    SlotId slot,
    RecordedNoteKind kind,
    const MidiMessage& message,
    Milliseconds offset
) {
    if (impl_->pending_events.size() >= max_recorded_events) {
        return;
    }

    const auto transposed = impl_->slot(slot).transpose(message);
    impl_->pending_events.push_back({
        .time_ms = static_cast<std::uint64_t>(offset.count()),
        .key = transposed.key,
        .velocity = transposed.velocity,
        .kind = kind,
    });
}

void Application::commitTake(SlotId slot, Milliseconds duration) {
    impl_->slot(slot).commitTake(impl_->pending_events, duration);
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
