#include "pending_take.hpp"

namespace zeta {

PendingTake::PendingTake() {
    events_.reserve(maximum_event_count);
}

void PendingTake::reset() {
    events_.clear();
    held_keys_.reset();
    content_duration_ = Milliseconds::zero();
}

void PendingTake::record(
    RecordedNoteKind kind,
    const MidiMessage& message,
    Milliseconds offset
) {
    const auto key = static_cast<std::size_t>(message.key);
    if (kind == RecordedNoteKind::NoteOn) {
        recordNoteOn(key, message, offset);
        return;
    }
    recordNoteOff(key, message, offset);
}

void PendingTake::finish(Milliseconds completion_offset) {
    for (std::size_t key = 0; key < midi_key_count; ++key) {
        if (!held_keys_.test(key)) {
            continue;
        }
        events_.push_back({
            .time_ms = static_cast<std::uint64_t>(completion_offset.count()),
            .key = static_cast<int>(key),
            .velocity = 0,
            .kind = RecordedNoteKind::NoteOff,
        });
    }
    if (held_keys_.any()) {
        content_duration_ = completion_offset;
    }
    held_keys_.reset();
}

const std::vector<RecordedLoopEvent>& PendingTake::events() const noexcept {
    return events_;
}

Milliseconds PendingTake::contentDuration() const noexcept {
    return content_duration_;
}

bool PendingTake::canRecordNoteOn(std::size_t key) const {
    const std::size_t newly_held = held_keys_.test(key) ? 0 : 1;
    return events_.size() + held_keys_.count() + newly_held + 1
        <= maximum_event_count;
}

void PendingTake::recordNoteOn(
    std::size_t key,
    const MidiMessage& message,
    Milliseconds offset
) {
    if (!canRecordNoteOn(key)) {
        return;
    }
    events_.push_back({
        .time_ms = static_cast<std::uint64_t>(offset.count()),
        .key = message.key,
        .velocity = message.velocity,
        .kind = RecordedNoteKind::NoteOn,
    });
    held_keys_.set(key);
}

void PendingTake::recordNoteOff(
    std::size_t key,
    const MidiMessage& message,
    Milliseconds offset
) {
    if (!held_keys_.test(key)) {
        return;
    }
    events_.push_back({
        .time_ms = static_cast<std::uint64_t>(offset.count()),
        .key = message.key,
        .velocity = message.velocity,
        .kind = RecordedNoteKind::NoteOff,
    });
    held_keys_.reset(key);
    content_duration_ = offset;
}

} // namespace zeta
