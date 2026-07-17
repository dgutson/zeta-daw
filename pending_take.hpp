#pragma once

#include "looper_fsm.hpp"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zeta {

struct RecordedLoopEvent {
    std::uint64_t time_ms{};
    int key{};
    int velocity{};
    RecordedNoteKind kind{RecordedNoteKind::NoteOff};
};

class PendingTake final {
public:
    PendingTake();

    void reset();
    void record(
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    );
    void finish(Milliseconds completion_offset);

    const std::vector<RecordedLoopEvent>& events() const noexcept;
    Milliseconds contentDuration() const noexcept;

private:
    static constexpr std::size_t maximum_event_count = 16384;
    static constexpr std::size_t midi_key_count = 128;

    bool canRecordNoteOn(std::size_t key) const;
    void recordNoteOn(
        std::size_t key,
        const MidiMessage& message,
        Milliseconds offset
    );
    void recordNoteOff(
        std::size_t key,
        const MidiMessage& message,
        Milliseconds offset
    );

    std::vector<RecordedLoopEvent> events_;
    std::bitset<midi_key_count> held_keys_;
    Milliseconds content_duration_{};
};

} // namespace zeta
