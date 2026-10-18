#pragma once

#include "looper_fsm.hpp"
#include "pending_take.hpp"

#include <vector>

namespace zeta {

struct SoundFontDefinition;

struct FinishedTake {
    // Refers to the recorder's events, which discardTake and record change.
    const std::vector<RecordedLoopEvent>& events;
    Milliseconds content_duration;
};

// Records one loop slot's take. The group builds one recorder per slot, and
// the slot calls it only through this interface.
class LoopTakeRecorder {
public:
    virtual ~LoopTakeRecorder() = default;

    virtual void discardTake() = 0;
    virtual void record(
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    ) = 0;
    virtual FinishedTake finishTake(const TakeTiming& timing) = 0;
    virtual void selectProgram(const SoundFontDefinition& soundfont) = 0;
};

} // namespace zeta
