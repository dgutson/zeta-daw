#include "midi_take_recorder.hpp"

namespace zeta {

MidiTakeRecorder::MidiTakeRecorder(PendingTake& pending_take) noexcept
    : pending_take_(pending_take) {}

void MidiTakeRecorder::discardTake() {
    pending_take_.reset();
}

void MidiTakeRecorder::record(
    RecordedNoteKind kind,
    const MidiMessage& message,
    Milliseconds offset
) {
    pending_take_.record(kind, message, offset);
}

FinishedTake MidiTakeRecorder::finishTake(const TakeTiming& timing) {
    const auto completion_offset = elapsedMilliseconds(
        timing.recording_started_at,
        timing.completed_at
    );
    pending_take_.finish(completion_offset);
    return {
        .events = pending_take_.events(),
        .content_duration = pending_take_.contentDuration(),
    };
}

} // namespace zeta
