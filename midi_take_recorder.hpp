#pragma once

#include "loop_take_recorder.hpp"
#include "looper_fsm.hpp"
#include "pending_take.hpp"

namespace zeta {

class SynthEngine;

class MidiTakeRecorder final : public LoopTakeRecorder {
public:
    MidiTakeRecorder(
        PendingTake& pending_take,
        SynthEngine& synth_engine,
        int channel
    ) noexcept;

private:
    void discardTake() override;
    void record(
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    ) override;
    FinishedTake finishTake(const TakeTiming& timing) override;
    void selectProgram(const SoundFontDefinition& soundfont) override;

    PendingTake& pending_take_;
    SynthEngine& synth_engine_;
    int channel_;
};

} // namespace zeta
