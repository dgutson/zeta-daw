#include "midi_loop_slot.hpp"

#include <stdexcept>
#include <utility>

namespace zeta {

MidiLoopSlot::MidiLoopSlot(
    SlotId id,
    const LoopSlotDefinition& definition,
    SynthEngine& synth_engine,
    PendingTake& pending_take
)
    : LoopSlot(id, definition, synth_engine),
      recorder_(pending_take) {}

void MidiLoopSlot::recordNote(
    RecordedNoteKind kind,
    const MidiMessage& message,
    Milliseconds offset
) {
    recorder_.record(kind, message, offset);
}

void MidiLoopSlot::complete(const TakeTiming& timing) {
    auto take = recorder_.finishTake(timing);
    // Arming does not clear the group's pending take, so it is emptied here,
    // before completion can throw.
    recorder_.discardTake();
    completeTake(timing, std::move(take));
}

GuideLoopSlot::GuideLoopSlot(
    SlotId id,
    const LoopSlotDefinition& definition,
    SynthEngine& synth_engine,
    PendingTake& pending_take,
    LoopSlotGroupOutput& output
)
    : MidiLoopSlot(id, definition, synth_engine, pending_take),
      output_(output) {}

LoopSlotSelectionOutcome GuideLoopSlot::onMutedSelection(
    const LoopSlotSelectionContext& context
) {
    return arm(context, std::nullopt);
}

void GuideLoopSlot::onLoopingSelection() {
    output_.stopDependentSlots();
    deactivate();
}

LoopPlaybackSchedule GuideLoopSlot::makeSchedule(
    const TakeTiming& timing,
    Milliseconds,
    const std::optional<LoopPlaybackSchedule>&
) const {
    return LoopPlaybackSchedule::forGuide(timing);
}

RegularLoopSlot::RegularLoopSlot(
    SlotId id,
    const LoopSlotDefinition& definition,
    SynthEngine& synth_engine,
    PendingTake& pending_take
) : MidiLoopSlot(id, definition, synth_engine, pending_take) {}

LoopSlotSelectionOutcome RegularLoopSlot::onMutedSelection(
    const LoopSlotSelectionContext& context
) {
    if (!context.guide_schedule) {
        return LoopSlotSelectionOutcome::GuideRequired;
    }
    return arm(context, context.guide_schedule);
}

void RegularLoopSlot::onLoopingSelection() {
    deactivate();
}

LoopPlaybackSchedule RegularLoopSlot::makeSchedule(
    const TakeTiming& timing,
    Milliseconds content_duration,
    const std::optional<LoopPlaybackSchedule>& guide
) const {
    if (!guide) {
        throw std::logic_error("Regular slot recording has no guide schedule");
    }
    return LoopPlaybackSchedule::forRegular(
        timing,
        content_duration,
        guide.value()
    );
}

} // namespace zeta
