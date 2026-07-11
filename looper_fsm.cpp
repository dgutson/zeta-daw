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
    StateId shutdownRequested(LooperStateData&, LooperOutput& output) const final {
        return stopApplication(output);
    }
};

class ReadyState final : public ActiveState {
public:
    StateId primaryControlPressed(
        LooperStateData&,
        LooperOutput& output,
        TimePoint
    ) const override {
        output.stopLoopPlayback();
        output.silenceAllChannels();
        output.resetTake();
        output.showRecordingArmed();
        return StateId::Armed;
    }

    MidiHandlingResult midiMessage(
        LooperStateData&,
        LooperOutput& output,
        MidiMessageType,
        MidiMessage& message,
        TimePoint
    ) const override {
        return {
            .next_state = StateId::Ready,
            .native_result = output.monitorMidi(
                message,
                MidiRoute::LiveChannel
            ),
        };
    }
};

class ArmedState final : public ActiveState {
public:
    StateId primaryControlPressed(
        LooperStateData&,
        LooperOutput& output,
        TimePoint
    ) const override {
        output.showNoTake();
        return stopApplication(output);
    }

    MidiHandlingResult midiMessage(
        LooperStateData& data,
        LooperOutput& output,
        MidiMessageType type,
        MidiMessage& message,
        TimePoint received_at
    ) const override {
        const int result = output.monitorMidi(message, MidiRoute::LoopChannel);

        if (type == MidiMessageType::NoteOn && message.velocity > 0) {
            data.recording_started_at = received_at;
            output.recordNote(
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
    StateId primaryControlPressed(
        LooperStateData& data,
        LooperOutput& output,
        TimePoint now
    ) const override {
        const auto duration = elapsedMilliseconds(data.recording_started_at, now);
        if (duration <= Milliseconds::zero()) {
            output.showNoTake();
            return stopApplication(output);
        }

        output.commitTake(duration);
        output.startLoopPlayback();
        output.showLooping();
        return StateId::Looping;
    }

    MidiHandlingResult midiMessage(
        LooperStateData& data,
        LooperOutput& output,
        MidiMessageType type,
        MidiMessage& message,
        TimePoint received_at
    ) const override {
        const int result = output.monitorMidi(message, MidiRoute::LoopChannel);
        const auto offset = elapsedMilliseconds(
            data.recording_started_at,
            received_at
        );

        if (type == MidiMessageType::NoteOn) {
            const auto kind = message.velocity > 0
                ? RecordedNoteKind::NoteOn
                : RecordedNoteKind::NoteOff;
            output.recordNote(kind, message, offset);
        } else if (type == MidiMessageType::NoteOff) {
            output.recordNote(RecordedNoteKind::NoteOff, message, offset);
        }

        return {
            .next_state = StateId::Recording,
            .native_result = result,
        };
    }
};

class LoopingState final : public ActiveState {
public:
    StateId primaryControlPressed(
        LooperStateData&,
        LooperOutput& output,
        TimePoint
    ) const override {
        return stopApplication(output);
    }

    MidiHandlingResult midiMessage(
        LooperStateData&,
        LooperOutput& output,
        MidiMessageType,
        MidiMessage& message,
        TimePoint
    ) const override {
        return {
            .next_state = StateId::Looping,
            .native_result = output.monitorMidi(
                message,
                MidiRoute::LiveChannel
            ),
        };
    }
};

class StoppedState final : public LooperState {
public:
    StateId primaryControlPressed(
        LooperStateData&,
        LooperOutput&,
        TimePoint
    ) const override {
        return StateId::Stopped;
    }

    MidiHandlingResult midiMessage(
        LooperStateData&,
        LooperOutput&,
        MidiMessageType,
        MidiMessage&,
        TimePoint
    ) const override {
        return {
            .next_state = StateId::Stopped,
            .native_result = midi_ok,
        };
    }

    StateId shutdownRequested(LooperStateData&, LooperOutput&) const override {
        return StateId::Stopped;
    }
};

} // namespace

LooperStateRegistry::LooperStateRegistry() {
    states_[stateIndex(StateId::Ready)] = std::make_unique<ReadyState>();
    states_[stateIndex(StateId::Armed)] = std::make_unique<ArmedState>();
    states_[stateIndex(StateId::Recording)] = std::make_unique<RecordingState>();
    states_[stateIndex(StateId::Looping)] = std::make_unique<LoopingState>();
    states_[stateIndex(StateId::Stopped)] = std::make_unique<StoppedState>();
}

LooperStateRegistry::~LooperStateRegistry() = default;

const LooperState& LooperStateRegistry::at(StateId id) const {
    const auto& state = states_.at(stateIndex(id));
    if (!state) {
        throw std::logic_error("Looper StateId has no registered implementation");
    }
    return *state;
}

LooperFsm::LooperFsm(LooperStateRegistry& states, LooperOutput& output) noexcept
    : states_(states), output_(output) {}

StateId LooperFsm::primaryControlPressed(TimePoint now) {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).primaryControlPressed(
        data_,
        output_,
        now
    );
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
        output_,
        type,
        message,
        received_at
    );
    install(result.next_state);
    return result.native_result;
}

StateId LooperFsm::shutdownRequested() {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).shutdownRequested(data_, output_);
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
