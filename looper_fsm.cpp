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

StateId stopApplication(LooperOutput& output) {
    output.stopLoopPlayback();
    output.silenceAllChannels();
    output.stopPlaybackWorker();
    return StateId::Stopped;
}

class ActiveState : public LooperState {
public:
    using LooperState::LooperState;

    StateId shutdownRequested(LooperStateData&) const final {
        return stopApplication(output_);
    }
};

class ReadyState final : public ActiveState {
public:
    using ActiveState::ActiveState;

    StateId primaryControlPressed(
        LooperStateData&,
        TimePoint
    ) const override {
        output_.stopLoopPlayback();
        output_.silenceAllChannels();
        output_.resetTake();
        output_.showRecordingArmed();
        return StateId::Armed;
    }

    MidiHandlingResult midiMessage(
        LooperStateData&,
        MidiMessageType,
        MidiMessage& message,
        TimePoint
    ) const override {
        return {
            .next_state = StateId::Ready,
            .native_result = output_.monitorMidi(
                message,
                MidiRoute::LiveChannel
            ),
        };
    }
};

class ArmedState final : public ActiveState {
public:
    using ActiveState::ActiveState;

    StateId primaryControlPressed(
        LooperStateData&,
        TimePoint
    ) const override {
        output_.showNoTake();
        return stopApplication(output_);
    }

    MidiHandlingResult midiMessage(
        LooperStateData& data,
        MidiMessageType type,
        MidiMessage& message,
        TimePoint received_at
    ) const override {
        const int result = output_.monitorMidi(message, MidiRoute::LoopChannel);

        if (type == MidiMessageType::NoteOn && message.velocity > 0) {
            data.recording_started_at = received_at;
            output_.recordNote(
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

class RecordingState final : public ActiveState {
public:
    using ActiveState::ActiveState;

    StateId primaryControlPressed(
        LooperStateData& data,
        TimePoint now
    ) const override {
        const auto duration = elapsedMilliseconds(data.recording_started_at, now);
        if (duration <= Milliseconds::zero()) {
            output_.showNoTake();
            return stopApplication(output_);
        }

        output_.commitTake(duration);
        output_.startLoopPlayback();
        output_.showLooping();
        return StateId::Looping;
    }

    MidiHandlingResult midiMessage(
        LooperStateData& data,
        MidiMessageType type,
        MidiMessage& message,
        TimePoint received_at
    ) const override {
        const int result = output_.monitorMidi(message, MidiRoute::LoopChannel);
        const auto offset = elapsedMilliseconds(
            data.recording_started_at,
            received_at
        );

        if (type == MidiMessageType::NoteOn) {
            const auto kind = message.velocity > 0
                ? RecordedNoteKind::NoteOn
                : RecordedNoteKind::NoteOff;
            output_.recordNote(kind, message, offset);
        } else if (type == MidiMessageType::NoteOff) {
            output_.recordNote(RecordedNoteKind::NoteOff, message, offset);
        }

        return {
            .next_state = StateId::Recording,
            .native_result = result,
        };
    }
};

class LoopingState final : public ActiveState {
public:
    using ActiveState::ActiveState;

    StateId primaryControlPressed(
        LooperStateData&,
        TimePoint
    ) const override {
        return stopApplication(output_);
    }

    MidiHandlingResult midiMessage(
        LooperStateData&,
        MidiMessageType,
        MidiMessage& message,
        TimePoint
    ) const override {
        return {
            .next_state = StateId::Looping,
            .native_result = output_.monitorMidi(
                message,
                MidiRoute::LiveChannel
            ),
        };
    }
};

class StoppedState final : public LooperState {
public:
    using LooperState::LooperState;

    StateId primaryControlPressed(
        LooperStateData&,
        TimePoint
    ) const override {
        return StateId::Stopped;
    }

    MidiHandlingResult midiMessage(
        LooperStateData&,
        MidiMessageType,
        MidiMessage&,
        TimePoint
    ) const override {
        return {
            .next_state = StateId::Stopped,
            .native_result = midi_ok,
        };
    }

    StateId shutdownRequested(LooperStateData&) const override {
        return StateId::Stopped;
    }
};

} // namespace

LooperState::LooperState(LooperOutput& output) noexcept : output_(output) {}

LooperStateRegistry::LooperStateRegistry(LooperOutput& output) {
    states_[stateIndex(StateId::Ready)] = std::make_unique<ReadyState>(output);
    states_[stateIndex(StateId::Armed)] = std::make_unique<ArmedState>(output);
    states_[stateIndex(StateId::Recording)] = std::make_unique<RecordingState>(output);
    states_[stateIndex(StateId::Looping)] = std::make_unique<LoopingState>(output);
    states_[stateIndex(StateId::Stopped)] = std::make_unique<StoppedState>(output);
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

StateId LooperFsm::primaryControlPressed(TimePoint now) {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).primaryControlPressed(data_, now);
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
        data_,
        type,
        message,
        received_at
    );
    install(result.next_state);
    return result.native_result;
}

StateId LooperFsm::shutdownRequested() {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).shutdownRequested(data_);
    install(next);
    return current_state_;
}

bool LooperFsm::shouldRun() const {
    std::lock_guard lock(mutex_);
    return !isTerminal(current_state_);
}

StateId LooperFsm::stateId() const {
    std::lock_guard lock(mutex_);
    return current_state_;
}

void LooperFsm::install(StateId next_state) {
    states_.at(next_state);
    current_state_ = next_state;
}

MidiMessageType classifyMidiMessage(int raw_type) noexcept {
    switch (raw_type) {
    case static_cast<int>(MidiMessageType::NoteOff):
        return MidiMessageType::NoteOff;
    case static_cast<int>(MidiMessageType::NoteOn):
        return MidiMessageType::NoteOn;
    case static_cast<int>(MidiMessageType::ControlChange):
        return MidiMessageType::ControlChange;
    case static_cast<int>(MidiMessageType::ProgramChange):
        return MidiMessageType::ProgramChange;
    case static_cast<int>(MidiMessageType::PitchBend):
        return MidiMessageType::PitchBend;
    default:
        return MidiMessageType::Other;
    }
}

bool isTerminal(StateId id) noexcept {
    return id == StateId::Stopped;
}

} // namespace zeta
