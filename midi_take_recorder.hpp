#pragma once

#include "looper_fsm.hpp"

namespace zeta {

class PendingTake;

class MidiTakeRecorder final {
public:
    explicit MidiTakeRecorder(PendingTake& pending_take) noexcept;

    void record(
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    );

private:
    PendingTake& pending_take_;
};

} // namespace zeta
