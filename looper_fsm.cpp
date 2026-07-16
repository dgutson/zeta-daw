#include "looper_fsm.hpp"

#include <stdexcept>

namespace zeta {
namespace {

constexpr int midi_ok = 0;

std::size_t stateIndex(StateId id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= static_cast<std::size_t>(StateId::Count)) {
        throw std::out_of_range("Invalid looper StateId");
    }
    return index;
}

Milliseconds elapsedMilliseconds(TimePoint start, TimePoint finish) {
    const auto elapsed = std::chrono::duration_cast<Milliseconds>(finish - start);
    return elapsed < Milliseconds::zero() ? Milliseconds::zero() : elapsed;
}

SlotId recordingSlot(const LooperStateData& data) {
    if (!data.recording_slot) {
        throw std::logic_error("Looper recording state has no selected slot");
    }
    return data.recording_slot.value();
}

StateId shutdownApplication(LooperOutput& output) {
    output.terminateSlots();
    output.silenceAllChannels();
    return StateId::Stopped;
}

void startOrMuteSlot(
    LooperOutput& output,
    const LoopSlotView& slots,
    SlotId slot
) {
    if (slots.slotPlaybackState(slot) == SlotPlaybackState::Looping) {
        output.muteSlotPlayback(slot);
        output.showMuted(slot);
    } else {
        output.startSlotPlayback(slot);
        output.showLooping(slot);
    }
}

class ActiveState : public LooperState {
public:
    using LooperState::LooperState;

    StateId shutdownRequested() const final {
        return shutdownApplication(output_);
    }
};

class ReadyState : public ActiveState {
public:
    using ActiveState::ActiveState;

    StateId loopSlotControlPressed(TimePoint) const override {
        return StateId::ReadySelectingLoopSlot;
    }

    StateId nextSoundFontPressed() const override {
        output_.selectNextSoundFont(MidiRoute::live());
        return StateId::Ready;
    }

    StateId soundFontByNotePressed() const override {
        return StateId::ReadySelectingSoundFont;
    }

    StateId octaveDownPressed() const override {
        output_.octaveDown(MidiRoute::live());
        return StateId::Ready;
    }

    StateId octaveUpPressed() const override {
        output_.octaveUp(MidiRoute::live());
        return StateId::Ready;
    }

    MidiHandlingResult midiMessage(
        MidiMessageType,
        MidiMessage& message,
        TimePoint
    ) const override {
        return {
            .next_state = StateId::Ready,
            .native_result = output_.monitorMidi(message, MidiRoute::live()),
        };
    }
};

class ReadySelectingSoundFontState final : public ReadyState {
public:
    using ReadyState::ReadyState;

    StateId soundFontByNotePressed() const override {
        return StateId::Ready;
    }

    MidiHandlingResult midiMessage(
        MidiMessageType type,
        MidiMessage& message,
        TimePoint
    ) const override {
        if (type == MidiMessageType::NoteOn && message.velocity > 0) {
            output_.selectSoundFontByNote(MidiRoute::live(), message.key);
            return {.next_state = StateId::Ready, .native_result = midi_ok};
        }

        return {
            .next_state = StateId::ReadySelectingSoundFont,
            .native_result = output_.monitorMidi(message, MidiRoute::live()),
        };
    }
};

class ReadySelectingLoopSlotState final : public ReadyState {
public:
    using ReadyState::ReadyState;

    StateId loopSlotControlPressed(TimePoint) const override {
        return StateId::Ready;
    }

    MidiHandlingResult midiMessage(
        MidiMessageType type,
        MidiMessage& message,
        TimePoint
    ) const override {
        if (type != MidiMessageType::NoteOn || message.velocity <= 0) {
            return {
                .next_state = StateId::ReadySelectingLoopSlot,
                .native_result = output_.monitorMidi(message, MidiRoute::live()),
            };
        }

        const auto slot = slots_.slotByKey(message.key);
        if (!slot) {
            output_.showUnknownLoopSlot(message.key);
            return {.next_state = StateId::Ready, .native_result = midi_ok};
        }

        if (slots_.slotHasTake(slot.value())) {
            startOrMuteSlot(output_, slots_, slot.value());
            return {.next_state = StateId::Ready, .native_result = midi_ok};
        }

        data_.recording_slot = slot;
        output_.prepareTake(slot.value());
        output_.showRecordingArmed(slot.value());
        return {.next_state = StateId::Armed, .native_result = midi_ok};
    }
};

class ArmedState : public ActiveState {
public:
    using ActiveState::ActiveState;

    StateId loopSlotControlPressed(TimePoint) const override {
        const SlotId slot = recordingSlot(data_);
        output_.discardPendingTake(slot);
        output_.selectCurrentSoundFont(MidiRoute::live());
        output_.showNoTake(slot);
        data_.recording_slot.reset();
        return StateId::Ready;
    }

    StateId nextSoundFontPressed() const override {
        output_.selectNextSoundFont(
            MidiRoute::recordingSlot(recordingSlot(data_))
        );
        return StateId::Armed;
    }

    StateId soundFontByNotePressed() const override {
        return StateId::ArmedSelectingSoundFont;
    }

    StateId octaveDownPressed() const override {
        output_.octaveDown(MidiRoute::live());
        output_.octaveDown(MidiRoute::recordingSlot(recordingSlot(data_)));
        return StateId::Armed;
    }

    StateId octaveUpPressed() const override {
        output_.octaveUp(MidiRoute::live());
        output_.octaveUp(MidiRoute::recordingSlot(recordingSlot(data_)));
        return StateId::Armed;
    }

    MidiHandlingResult midiMessage(
        MidiMessageType type,
        MidiMessage& message,
        TimePoint received_at
    ) const override {
        const SlotId slot = recordingSlot(data_);
        const auto route = MidiRoute::recordingSlot(slot);
        const int result = output_.monitorMidi(message, route);

        if (type == MidiMessageType::NoteOn && message.velocity > 0) {
            data_.recording_started_at = received_at;
            output_.recordNote(
                slot,
                RecordedNoteKind::NoteOn,
                message,
                Milliseconds::zero()
            );
            return {.next_state = StateId::Recording, .native_result = result};
        }

        return {.next_state = StateId::Armed, .native_result = result};
    }
};

class ArmedSelectingSoundFontState final : public ArmedState {
public:
    using ArmedState::ArmedState;

    StateId soundFontByNotePressed() const override {
        return StateId::Armed;
    }

    MidiHandlingResult midiMessage(
        MidiMessageType type,
        MidiMessage& message,
        TimePoint
    ) const override {
        const auto route = MidiRoute::recordingSlot(recordingSlot(data_));
        if (type == MidiMessageType::NoteOn && message.velocity > 0) {
            output_.selectSoundFontByNote(route, message.key);
            return {.next_state = StateId::Armed, .native_result = midi_ok};
        }

        return {
            .next_state = StateId::ArmedSelectingSoundFont,
            .native_result = output_.monitorMidi(message, route),
        };
    }
};

class RecordingState : public ActiveState {
public:
    using ActiveState::ActiveState;

    StateId loopSlotControlPressed(TimePoint received_at) const override {
        const SlotId slot = recordingSlot(data_);
        const auto duration = elapsedMilliseconds(
            data_.recording_started_at,
            received_at
        );
        output_.commitTake(slot, duration);
        output_.selectCurrentSoundFont(MidiRoute::live());
        output_.startSlotPlayback(slot);
        output_.showLooping(slot);
        data_.recording_slot.reset();
        return StateId::Ready;
    }

    StateId nextSoundFontPressed() const override {
        return StateId::Recording;
    }

    StateId soundFontByNotePressed() const override {
        return StateId::Recording;
    }

    StateId octaveDownPressed() const override {
        return StateId::Recording;
    }

    StateId octaveUpPressed() const override {
        return StateId::Recording;
    }

    MidiHandlingResult midiMessage(
        MidiMessageType type,
        MidiMessage& message,
        TimePoint received_at
    ) const override {
        const SlotId slot = recordingSlot(data_);
        const int result = output_.monitorMidi(
            message,
            MidiRoute::recordingSlot(slot)
        );
        const auto offset = elapsedMilliseconds(
            data_.recording_started_at,
            received_at
        );

        if (type == MidiMessageType::NoteOn) {
            const auto kind = message.velocity > 0
                ? RecordedNoteKind::NoteOn
                : RecordedNoteKind::NoteOff;
            output_.recordNote(slot, kind, message, offset);
        } else if (type == MidiMessageType::NoteOff) {
            output_.recordNote(
                slot,
                RecordedNoteKind::NoteOff,
                message,
                offset
            );
        }

        return {.next_state = StateId::Recording, .native_result = result};
    }
};

class StoppedState final : public LooperState {
public:
    using LooperState::LooperState;

    StateId loopSlotControlPressed(TimePoint) const override {
        return StateId::Stopped;
    }
    StateId nextSoundFontPressed() const override { return StateId::Stopped; }
    StateId soundFontByNotePressed() const override { return StateId::Stopped; }
    StateId octaveDownPressed() const override { return StateId::Stopped; }
    StateId octaveUpPressed() const override { return StateId::Stopped; }

    MidiHandlingResult midiMessage(
        MidiMessageType,
        MidiMessage&,
        TimePoint
    ) const override {
        return {.next_state = StateId::Stopped, .native_result = midi_ok};
    }

    StateId shutdownRequested() const override { return StateId::Stopped; }
};

} // namespace

MidiRoute MidiRoute::live() noexcept {
    return {.kind = MidiRouteKind::Live};
}

MidiRoute MidiRoute::recordingSlot(SlotId slot) noexcept {
    return {.kind = MidiRouteKind::RecordingSlot, .slot = slot};
}

LooperState::LooperState(
    LooperOutput& output,
    const LoopSlotView& slots,
    LooperStateData& data
) noexcept : output_(output), slots_(slots), data_(data) {}

LooperStateRegistry::LooperStateRegistry(
    LooperOutput& output,
    const LoopSlotView& slots
) {
    states_[stateIndex(StateId::Ready)] =
        std::make_unique<ReadyState>(output, slots, data_);
    states_[stateIndex(StateId::ReadySelectingSoundFont)] =
        std::make_unique<ReadySelectingSoundFontState>(output, slots, data_);
    states_[stateIndex(StateId::ReadySelectingLoopSlot)] =
        std::make_unique<ReadySelectingLoopSlotState>(output, slots, data_);
    states_[stateIndex(StateId::Armed)] =
        std::make_unique<ArmedState>(output, slots, data_);
    states_[stateIndex(StateId::ArmedSelectingSoundFont)] =
        std::make_unique<ArmedSelectingSoundFontState>(output, slots, data_);
    states_[stateIndex(StateId::Recording)] =
        std::make_unique<RecordingState>(output, slots, data_);
    states_[stateIndex(StateId::Stopped)] =
        std::make_unique<StoppedState>(output, slots, data_);
}

LooperStateRegistry::~LooperStateRegistry() = default;

const LooperState& LooperStateRegistry::at(StateId id) const {
    const auto& state = states_.at(stateIndex(id));
    if (!state) {
        throw std::logic_error("Looper StateId has no registered implementation");
    }
    return *state;
}

LooperFsm::LooperFsm(LooperStateRegistry& states) noexcept : states_(states) {}

StateId LooperFsm::loopSlotControlPressed(TimePoint received_at) {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).loopSlotControlPressed(
        received_at
    );
    install(next);
    return current_state_;
}

StateId LooperFsm::nextSoundFontPressed() {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).nextSoundFontPressed();
    install(next);
    return current_state_;
}

StateId LooperFsm::soundFontByNotePressed() {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).soundFontByNotePressed();
    install(next);
    return current_state_;
}

StateId LooperFsm::octaveDownPressed() {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).octaveDownPressed();
    install(next);
    return current_state_;
}

StateId LooperFsm::octaveUpPressed() {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).octaveUpPressed();
    install(next);
    return current_state_;
}

int LooperFsm::midiMessage(
    MidiMessageType type,
    MidiMessage message,
    TimePoint received_at
) {
    std::lock_guard lock(mutex_);
    const MidiHandlingResult result = states_.at(current_state_).midiMessage(
        type,
        message,
        received_at
    );
    install(result.next_state);
    return result.native_result;
}

StateId LooperFsm::shutdownRequested() {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).shutdownRequested();
    install(next);
    return current_state_;
}

bool LooperFsm::shouldRun() const {
    std::lock_guard lock(mutex_);
    return current_state_ != StateId::Stopped;
}

StateId LooperFsm::stateId() const {
    std::lock_guard lock(mutex_);
    return current_state_;
}

void LooperFsm::install(StateId next_state) {
    states_.at(next_state);
    current_state_ = next_state;
}

} // namespace zeta
