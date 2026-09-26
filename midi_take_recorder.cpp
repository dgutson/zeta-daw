#include "midi_take_recorder.hpp"

#include "synth_engine.hpp"

namespace zeta {

MidiTakeRecorder::MidiTakeRecorder(
    PendingTake& pending_take,
    SynthEngine& synth_engine,
    int channel
) noexcept
    : pending_take_(pending_take),
      synth_engine_(synth_engine),
      channel_(channel) {}

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

void MidiTakeRecorder::selectProgram(const SoundFontDefinition& soundfont) {
    synth_engine_.select(soundfont, channel_);
}

} // namespace zeta
