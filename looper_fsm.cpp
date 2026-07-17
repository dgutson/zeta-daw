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

StateId shutdownApplication(LooperOutput& output) {
    output.terminateLoopSlots();
    output.silenceAllChannels();
    return StateId::Stopped;
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
        output_.selectNextLiveSoundFont();
        return StateId::Ready;
    }

    StateId soundFontByNotePressed() const override {
        return StateId::ReadySelectingSoundFont;
    }

    StateId octaveDownPressed() const override {
        output_.octaveDownLive();
        return StateId::Ready;
    }

    StateId octaveUpPressed() const override {
        output_.octaveUpLive();
        return StateId::Ready;
    }

    MidiHandlingResult midiMessage(
        MidiMessageType,
        MidiMessage& message,
        TimePoint
    ) const override {
        return {
            .next_state = StateId::Ready,
            .native_result = output_.monitorLiveMidi(message),
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
        if (type == MidiMessageType::NoteOn && message.velocity > 0) {
            const auto selection = output_.requestLoopSlotSelection(message.key);
            if (selection.outcome != LoopSlotSelectionOutcome::Armed) {
                return {
                    .next_state = StateId::Ready,
                    .native_result = midi_ok,
                };
            }

            data_.selected_recording_slot = selection.id;
            output_.showRecordingArmed(selection.id);
            return {
                .next_state = StateId::Armed,
                .native_result = midi_ok,
            };
        }

        return {
            .next_state = StateId::ReadySelectingLoopSlot,
            .native_result = output_.monitorLiveMidi(message),
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
            output_.selectLiveSoundFontByNote(message.key);
            return {
                .next_state = StateId::Ready,
                .native_result = midi_ok,
            };
        }

        return {
            .next_state = StateId::ReadySelectingSoundFont,
            .native_result = output_.monitorLiveMidi(message),
        };
    }
};

class ArmedState : public ActiveState {
public:
    using ActiveState::ActiveState;

    StateId loopSlotControlPressed(TimePoint) const override {
        const SlotId slot = selectedRecordingSlot();
        output_.cancelLoopSlotRecording(slot);
        output_.selectCurrentLiveSoundFont();
        output_.showNoTake(slot);
        data_.selected_recording_slot.reset();
        return StateId::Ready;
    }

    StateId nextSoundFontPressed() const override {
        output_.selectNextLoopSlotSoundFont(selectedRecordingSlot());
        return StateId::Armed;
    }

    StateId soundFontByNotePressed() const override {
        return StateId::ArmedSelectingSoundFont;
    }

    StateId octaveDownPressed() const override {
        output_.octaveDownLive();
        output_.octaveDownLoopSlot(selectedRecordingSlot());
        return StateId::Armed;
    }

    StateId octaveUpPressed() const override {
        output_.octaveUpLive();
        output_.octaveUpLoopSlot(selectedRecordingSlot());
        return StateId::Armed;
    }

    MidiHandlingResult midiMessage(
        MidiMessageType type,
        MidiMessage& message,
        TimePoint received_at
    ) const override {
        const SlotId slot = selectedRecordingSlot();
        const int result = output_.monitorLoopSlotMidi(slot, message);

        if (type == MidiMessageType::NoteOn && message.velocity > 0) {
            data_.recording_started_at = received_at;
            output_.recordNote(
                slot,
                RecordedNoteKind::NoteOn,
                message,
                Milliseconds::zero()
            );
            return {
                .next_state = StateId::Recording,
                .native_result = result,
            };
        }

        return {
            .next_state = StateId::Armed,
            .native_result = result,
        };
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
        const SlotId slot = selectedRecordingSlot();
        if (type == MidiMessageType::NoteOn && message.velocity > 0) {
            output_.selectLoopSlotSoundFontByNote(slot, message.key);
            return {
                .next_state = StateId::Armed,
                .native_result = midi_ok,
            };
        }

        return {
            .next_state = StateId::ArmedSelectingSoundFont,
            .native_result = output_.monitorLoopSlotMidi(slot, message),
        };
    }
};

class RecordingState final : public ActiveState {
public:
    using ActiveState::ActiveState;

    StateId loopSlotControlPressed(TimePoint now) const override {
        const SlotId slot = selectedRecordingSlot();
        output_.completeLoopSlotRecording(slot, TakeTiming{
            .recording_started_at = data_.recording_started_at,
            .completed_at = now,
        });
        output_.selectCurrentLiveSoundFont();
        output_.showLooping(slot);
        data_.selected_recording_slot.reset();
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
        const SlotId slot = selectedRecordingSlot();
        const int result = output_.monitorLoopSlotMidi(slot, message);
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

        return {
            .next_state = StateId::Recording,
            .native_result = result,
        };
    }
};

class StoppedState final : public LooperState {
public:
    using LooperState::LooperState;

    StateId loopSlotControlPressed(TimePoint) const override {
        return StateId::Stopped;
    }

    StateId nextSoundFontPressed() const override {
        return StateId::Stopped;
    }

    StateId soundFontByNotePressed() const override {
        return StateId::Stopped;
    }

    StateId octaveDownPressed() const override {
        return StateId::Stopped;
    }

    StateId octaveUpPressed() const override {
        return StateId::Stopped;
    }

    MidiHandlingResult midiMessage(
        MidiMessageType,
        MidiMessage&,
        TimePoint
    ) const override {
        return {
            .next_state = StateId::Stopped,
            .native_result = midi_ok,
        };
    }

    StateId shutdownRequested() const override {
        return StateId::Stopped;
    }
};

} // namespace

LooperState::LooperState(
    LooperOutput& output,
    LooperStateData& data
) noexcept : output_(output), data_(data) {}

SlotId LooperState::selectedRecordingSlot() const {
    if (!data_.selected_recording_slot) {
        throw std::logic_error("Looper state has no selected recording slot");
    }
    return data_.selected_recording_slot.value();
}

LooperStateRegistry::LooperStateRegistry(LooperOutput& output) {
    states_[stateIndex(StateId::Ready)] =
        std::make_unique<ReadyState>(output, data_);
    states_[stateIndex(StateId::ReadySelectingLoopSlot)] =
        std::make_unique<ReadySelectingLoopSlotState>(output, data_);
    states_[stateIndex(StateId::ReadySelectingSoundFont)] =
        std::make_unique<ReadySelectingSoundFontState>(output, data_);
    states_[stateIndex(StateId::Armed)] =
        std::make_unique<ArmedState>(output, data_);
    states_[stateIndex(StateId::ArmedSelectingSoundFont)] =
        std::make_unique<ArmedSelectingSoundFontState>(output, data_);
    states_[stateIndex(StateId::Recording)] =
        std::make_unique<RecordingState>(output, data_);
    states_[stateIndex(StateId::Stopped)] =
        std::make_unique<StoppedState>(output, data_);
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

StateId LooperFsm::loopSlotControlPressed(TimePoint now) {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).loopSlotControlPressed(now);
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
