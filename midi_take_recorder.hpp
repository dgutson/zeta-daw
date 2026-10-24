#pragma once

#include "looper_fsm.hpp"
#include "pending_take.hpp"
#include "take_player.hpp"

namespace zeta {

class MidiTakeRecorder final {
public:
    explicit MidiTakeRecorder(PendingTake& pending_take) noexcept;

    void discardTake();
    void record(
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    );
    FinishedTake finishTake(const TakeTiming& timing);

private:
    PendingTake& pending_take_;
};

} // namespace zeta
