#include "application.hpp"

#include "loop_slot.hpp"
#include "octave_transposer.hpp"
#include "soundfont_selector.hpp"
#include "synth_engine.hpp"

#include <array>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <syncstream>
#include <utility>
#include <vector>

namespace zeta {
namespace {

constexpr int live_channel = 0;
constexpr int first_loop_channel = 1;
constexpr int expression_controller = 11;
constexpr std::size_t midi_key_count = 128;

} // namespace

struct Application::Impl {
    SynthEngine synth_engine;
    SoundFontSelector soundfont_selector;
    OctaveTransposer live_transposer;
    std::vector<std::unique_ptr<LoopSlot>> slots;
    std::array<std::optional<SlotId>, midi_key_count> slots_by_key{};

    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_changed;

    explicit Impl(const ApplicationConfig& config)
        : synth_engine(config),
          soundfont_selector(config.soundfonts) {
        slots.reserve(config.loop_slot_keys.size());
        for (SlotId id = 0; id < config.loop_slot_keys.size(); ++id) {
            const int key = config.loop_slot_keys[id];
            slots_by_key[static_cast<std::size_t>(key)] = id;
            slots.push_back(std::make_unique<LoopSlot>(
                LoopSlotConfig{
                    .id = id,
                    .selection_key = key,
                    .synth_channel = first_loop_channel + static_cast<int>(id),
                },
                synth_engine
            ));
        }
        selectCurrentSoundFont(MidiRoute::live());
    }

    LoopSlot& slot(SlotId id) {
        return *slots.at(id);
    }

    const LoopSlot& slot(SlotId id) const {
        return *slots.at(id);
    }

    int monitorMidi(const MidiMessage& message, MidiRoute route) {
        if (route.kind == MidiRouteKind::Live) {
            return synth_engine.send(
                live_transposer.transpose(message),
                live_channel
            );
        }
        return slot(route.slot).monitorMidi(message);
    }

    void selectCurrentSoundFont(MidiRoute route) {
        selectSoundFont(soundfont_selector.current(), route);
    }

    void selectNextSoundFont(MidiRoute route) {
        selectSoundFont(soundfont_selector.next(), route);
        reportCurrentSoundFont();
    }

    void selectSoundFontByNote(MidiRoute route, int key) {
        const auto* soundfont = soundfont_selector.selectByKey(key);
        if (!soundfont) {
            std::cerr << "No SoundFont mapped for MIDI key " << key << '\n';
            return;
        }

        selectSoundFont(*soundfont, route);
        reportCurrentSoundFont();
    }

    void selectSoundFont(
        const SoundFontDefinition& soundfont,
        MidiRoute route
    ) {
        if (route.kind == MidiRouteKind::Live) {
            synth_engine.select(soundfont, live_channel);
        } else {
            slot(route.slot).selectSoundFont(soundfont);
        }
    }

    void reportCurrentSoundFont() const {
        std::cout << "SoundFont selected: "
                  << soundfont_selector.current().id << '\n';
    }

    void octaveDown(MidiRoute route) {
        if (route.kind == MidiRouteKind::Live) {
            live_transposer.octaveDown();
        } else {
            slot(route.slot).octaveDown();
        }
    }

    void octaveUp(MidiRoute route) {
        if (route.kind == MidiRouteKind::Live) {
            live_transposer.octaveUp();
        } else {
            slot(route.slot).octaveUp();
        }
    }

    void prepareTake(SlotId id) {
        slot(id).prepareTake(soundfont_selector.current(), live_transposer);
    }

    void terminateSlots() {
        for (auto& current : slots) {
            current->terminationRequested();
        }
    }

    void notifyLifecycleChanged() {
        lifecycle_changed.notify_all();
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
      states_(*this, *this),
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
        << "Use the configured loop-slot control, then a keyed note, to arm, "
        << "start, or mute a loop slot.\n";

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

        if (config_.loop_slot_control.matches(type, message)) {
            fsm_.loopSlotControlPressed();
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

std::optional<SlotId> Application::slotByKey(int key) const {
    if (key < 0 || key >= static_cast<int>(midi_key_count)) {
        return std::nullopt;
    }
    return impl_->slots_by_key[static_cast<std::size_t>(key)];
}

bool Application::slotHasTake(SlotId slot) const {
    return impl_->slot(slot).hasTake();
}

SlotPlaybackState Application::slotPlaybackState(SlotId slot) const {
    return impl_->slot(slot).playbackState();
}

int Application::monitorMidi(const MidiMessage& message, MidiRoute route) {
    const int result = impl_->monitorMidi(message, route);

    #ifdef ZETA_MIDI_TRACE
    std::osyncstream{std::cerr}
        << "[midi monitor]"
        << " route=" << (
            route.kind == MidiRouteKind::Live ? "live" : "recording-slot"
        )
        << " slot=" << route.slot
        << " input_channel=" << message.channel
        << " result=" << result << '\n';
    #endif

    return result;
}

void Application::selectCurrentSoundFont(MidiRoute route) {
    impl_->selectCurrentSoundFont(route);
}

void Application::selectNextSoundFont(MidiRoute route) {
    impl_->selectNextSoundFont(route);
}

void Application::selectSoundFontByNote(MidiRoute route, int key) {
    impl_->selectSoundFontByNote(route, key);
}

void Application::octaveDown(MidiRoute route) {
    impl_->octaveDown(route);
}

void Application::octaveUp(MidiRoute route) {
    impl_->octaveUp(route);
}

void Application::prepareTake(SlotId slot) {
    impl_->prepareTake(slot);
}

void Application::discardPendingTake(SlotId slot) {
    impl_->slot(slot).discardPendingTake();
}

void Application::recordNote(
    SlotId slot,
    RecordedNoteKind kind,
    const MidiMessage& message,
    Milliseconds offset
) {
    impl_->slot(slot).recordNote(kind, message, offset);
}

void Application::commitTake(SlotId slot, Milliseconds duration) {
    impl_->slot(slot).commitTake(duration);
}

void Application::startSlotPlayback(SlotId slot) {
    impl_->slot(slot).startRequested();
}

void Application::muteSlotPlayback(SlotId slot) {
    impl_->slot(slot).muteRequested();
}

void Application::terminateSlots() {
    impl_->terminateSlots();
}

void Application::silenceAllChannels() {
    impl_->allNotesOff();
}

void Application::showRecordingArmed(SlotId slot) {
    std::cout
        << "Recording... Loop slot " << slot << " armed; play a note to start.\n";
}

void Application::showLooping(SlotId slot) {
    std::cout
        << "Looping. Loop slot " << slot << " is active.\n"
        << "Use the configured loop-slot control followed by its key to mute "
        << "or resume it.\n";
}

void Application::showMuted(SlotId slot) {
    std::cout << "Loop slot " << slot << " muted.\n";
}

void Application::showNoTake(SlotId slot) {
    std::cout
        << "No notes were recorded. Ready for a new loop. Loop slot "
        << slot << " recording canceled.\n";
}

void Application::showRecorderBusy(SlotId slot) {
    std::cout
        << "Loop slot " << slot
        << " is empty; another slot already owns the recorder.\n";
}

void Application::showUnknownLoopSlot(int key) {
    std::cerr << "No loop slot mapped for MIDI key " << key << '\n';
}

} // namespace zeta
