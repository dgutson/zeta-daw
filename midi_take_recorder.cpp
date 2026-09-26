#include "midi_take_recorder.hpp"

#include "pending_take.hpp"

namespace zeta {

MidiTakeRecorder::MidiTakeRecorder(
    PendingTake& pending_take
) noexcept : pending_take_(pending_take) {}

void MidiTakeRecorder::record(
    RecordedNoteKind kind,
    const MidiMessage& message,
    Milliseconds offset
) {
    pending_take_.record(kind, message, offset);
}

} // namespace zeta
