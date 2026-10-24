#pragma once

#include "loop_slot.hpp"
#include "midi_take_recorder.hpp"
#include "pending_take.hpp"

#include <optional>

namespace zeta {

// A loop slot whose take is the notes played while it records.
class MidiLoopSlot : public LoopSlot {
public:
    void recordNote(
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    );
    void complete(const TakeTiming& timing);

protected:
    MidiLoopSlot(
        SlotId id,
        const LoopSlotDefinition& definition,
        SynthEngine& synth_engine,
        PendingTake& pending_take
    );

private:
    MidiTakeRecorder recorder_;
};

class GuideLoopSlot final : public MidiLoopSlot {
public:
    GuideLoopSlot(
        SlotId id,
        const LoopSlotDefinition& definition,
        SynthEngine& synth_engine,
        PendingTake& pending_take,
        LoopSlotGroupOutput& output
    );

private:
    LoopSlotSelectionOutcome onMutedSelection(
        const LoopSlotSelectionContext& context
    ) override;
    void onLoopingSelection() override;
    LoopPlaybackSchedule makeSchedule(
        const TakeTiming& timing,
        Milliseconds content_duration,
        const std::optional<LoopPlaybackSchedule>& guide
    ) const override;

    LoopSlotGroupOutput& output_;
};

class RegularLoopSlot final : public MidiLoopSlot {
public:
    RegularLoopSlot(
        SlotId id,
        const LoopSlotDefinition& definition,
        SynthEngine& synth_engine,
        PendingTake& pending_take
    );

private:
    LoopSlotSelectionOutcome onMutedSelection(
        const LoopSlotSelectionContext& context
    ) override;
    void onLoopingSelection() override;
    LoopPlaybackSchedule makeSchedule(
        const TakeTiming& timing,
        Milliseconds content_duration,
        const std::optional<LoopPlaybackSchedule>& guide
    ) const override;
};

} // namespace zeta
