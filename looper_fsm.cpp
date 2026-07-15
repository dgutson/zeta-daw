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

void stopPlayback(LooperOutput& output) {
    output.stopLoopPlayback();
    output.silenceAllChannels();
}

StateId shutdownApplication(LooperOutput& output) {
    stopPlayback(output);
    output.stopPlaybackWorker();
    return StateId::Stopped;
}

class ActiveState : public LooperState {
public:
    using LooperState::LooperState;

    StateId shutdownRequested() const final {
        return shutdownApplication(output_);
    }
};

class ReadyState final : public ActiveState {
public:
    using ActiveState::ActiveState;

    StateId primaryControlPressed(TimePoint) const override {
        output_.stopLoopPlayback();
        output_.silenceAllChannels();
        output_.selectCurrentSoundFont(MidiRoute::LoopChannel);
        output_.resetTake();
        output_.showRecordingArmed();
        return StateId::Armed;
    }

    StateId nextSoundFontPressed() const override {
        output_.selectNextSoundFont(MidiRoute::LiveChannel);
        return StateId::Ready;
    }

    StateId octaveDownPressed() const override {
        output_.octaveDown(MidiRoute::LiveChannel);
        output_.octaveDown(MidiRoute::LoopChannel);
        return StateId::Ready;
    }

    StateId octaveUpPressed() const override {
        output_.octaveUp(MidiRoute::LiveChannel);
        output_.octaveUp(MidiRoute::LoopChannel);
        return StateId::Ready;
    }

    MidiHandlingResult midiMessage(
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

    StateId primaryControlPressed(TimePoint) const override {
        output_.showNoTake();
        stopPlayback(output_);
        output_.selectCurrentSoundFont(MidiRoute::LiveChannel);
        return StateId::Ready;
    }

    StateId nextSoundFontPressed() const override {
        output_.selectNextSoundFont(MidiRoute::LoopChannel);
        return StateId::Armed;
    }

    StateId octaveDownPressed() const override {
        output_.octaveDown(MidiRoute::LiveChannel);
        output_.octaveDown(MidiRoute::LoopChannel);
        return StateId::Armed;
    }

    StateId octaveUpPressed() const override {
        output_.octaveUp(MidiRoute::LiveChannel);
        output_.octaveUp(MidiRoute::LoopChannel);
        return StateId::Armed;
    }

    MidiHandlingResult midiMessage(
        MidiMessageType type,
        MidiMessage& message,
        TimePoint received_at
    ) const override {
        const int result = output_.monitorMidi(message, MidiRoute::LoopChannel);

        if (type == MidiMessageType::NoteOn && message.velocity > 0) {
            data_.recording_started_at = received_at;
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

    StateId primaryControlPressed(TimePoint now) const override {
        const auto duration = elapsedMilliseconds(
            data_.recording_started_at,
            now
        );
        output_.commitTake(duration);
        output_.startLoopPlayback();
        output_.showLooping();
        return StateId::Looping;
    }

    StateId nextSoundFontPressed() const override {
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
        const int result = output_.monitorMidi(message, MidiRoute::LoopChannel);
        const auto offset = elapsedMilliseconds(
            data_.recording_started_at,
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

    StateId primaryControlPressed(TimePoint) const override {
        stopPlayback(output_);
        return StateId::Ready;
    }

    StateId nextSoundFontPressed() const override {
        output_.selectNextSoundFont(MidiRoute::LiveChannel);
        return StateId::Looping;
    }

    StateId octaveDownPressed() const override {
        output_.octaveDown(MidiRoute::LiveChannel);
        return StateId::Looping;
    }

    StateId octaveUpPressed() const override {
        output_.octaveUp(MidiRoute::LiveChannel);
        return StateId::Looping;
    }

    MidiHandlingResult midiMessage(
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

    StateId primaryControlPressed(TimePoint) const override {
        return StateId::Stopped;
    }

    StateId nextSoundFontPressed() const override {
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

LooperStateRegistry::LooperStateRegistry(LooperOutput& output) {
    states_[stateIndex(StateId::Ready)] =
        std::make_unique<ReadyState>(output, data_);
    states_[stateIndex(StateId::Armed)] =
        std::make_unique<ArmedState>(output, data_);
    states_[stateIndex(StateId::Recording)] =
        std::make_unique<RecordingState>(output, data_);
    states_[stateIndex(StateId::Looping)] =
        std::make_unique<LoopingState>(output, data_);
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

StateId LooperFsm::primaryControlPressed(TimePoint now) {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).primaryControlPressed(now);
    install(next);
    return current_state_;
}

StateId LooperFsm::nextSoundFontPressed() {
    std::lock_guard lock(mutex_);
    const StateId next = states_.at(current_state_).nextSoundFontPressed();
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
