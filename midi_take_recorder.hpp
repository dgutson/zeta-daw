#pragma once

#include "looper_fsm.hpp"
#include "pending_take.hpp"

#include <vector>

namespace zeta {

class SynthEngine;
struct SoundFontDefinition;

struct FinishedTake {
    // Refers to the pending take's events, which discardTake and record change.
    const std::vector<RecordedLoopEvent>& events;
    Milliseconds content_duration;
};

class MidiTakeRecorder final {
public:
    MidiTakeRecorder(
        PendingTake& pending_take,
        SynthEngine& synth_engine,
        int channel
    ) noexcept;

    void discardTake();
    void record(
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    );
    FinishedTake finishTake(const TakeTiming& timing);
    void selectProgram(const SoundFontDefinition& soundfont);

private:
    PendingTake& pending_take_;
    SynthEngine& synth_engine_;
    int channel_;
};

} // namespace zeta
