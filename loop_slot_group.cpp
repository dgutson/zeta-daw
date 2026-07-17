#include "loop_slot_group.hpp"

#include "pending_take.hpp"
#include "synth_engine.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace zeta {
namespace {

constexpr std::size_t midi_key_count = 128;
constexpr SlotId guide_slot_id = 0;

} // namespace

struct LoopSlotGroup::Impl {
    std::vector<std::unique_ptr<LoopSlot>> slots;
    std::array<std::optional<SlotId>, midi_key_count> slots_by_key{};
    PendingTake pending_take;

    Impl(
        const std::vector<LoopSlotDefinition>& definitions,
        SynthEngine& synth_engine,
        LoopSlotGroupOutput& output
    ) {
        if (definitions.empty()) {
            throw std::invalid_argument("Loop-slot group requires a guide slot");
        }

        slots.reserve(definitions.size());
        auto guide = std::make_unique<GuideLoopSlot>(
            guide_slot_id,
            definitions.front(),
            synth_engine,
            output
        );
        addSlot(std::move(guide));

        for (SlotId id = 1; id < definitions.size(); ++id) {
            auto regular = std::make_unique<RegularLoopSlot>(
                id,
                definitions[id],
                synth_engine
            );
            addSlot(std::move(regular));
        }
    }

    void addSlot(std::unique_ptr<LoopSlot> slot) {
        slots_by_key[static_cast<std::size_t>(slot->selectionKey())] = slot->id();
        slots.push_back(std::move(slot));
    }

    LoopSlot& slot(SlotId id) {
        return *slots.at(id);
    }

    const LoopSlot& slot(SlotId id) const {
        return *slots.at(id);
    }

    LoopSlot& guide() {
        return slot(guide_slot_id);
    }
};

LoopSlotGroup::LoopSlotGroup(
    const std::vector<LoopSlotDefinition>& definitions,
    SynthEngine& synth_engine
) : impl_(std::make_unique<Impl>(
        definitions,
        synth_engine,
        static_cast<LoopSlotGroupOutput&>(*this)
    )) {}

LoopSlotGroup::~LoopSlotGroup() = default;

int LoopSlotGroup::channel(SlotId slot) const {
    const auto& group = *impl_;
    return group.slot(slot).channel();
}

int LoopSlotGroup::monitorMidi(
    SlotId slot,
    const MidiMessage& message
) {
    return impl_->slot(slot).monitorMidi(message);
}

LoopSlotSelectionResult LoopSlotGroup::requestSelection(
    int key,
    const SoundFontDefinition& soundfont,
    const OctaveTransposer& transposer
) {
    const auto id = impl_->slots_by_key.at(static_cast<std::size_t>(key));
    if (!id) {
        std::cerr << "No loop slot mapped for MIDI key " << key << '\n';
        return {.outcome = LoopSlotSelectionOutcome::Unavailable};
    }

    const LoopSlotSelectionContext context{
        .soundfont = soundfont,
        .transposer = transposer,
        .guide_schedule = impl_->guide().activeSchedule(),
    };
    const auto outcome = impl_->slot(id.value()).selectionRequested(context);
    if (outcome == LoopSlotSelectionOutcome::Armed) {
        impl_->pending_take.reset();
    } else if (outcome == LoopSlotSelectionOutcome::GuideRequired) {
        std::cerr << "Loop slot " << id.value() + 1
                  << " requires the guide to be looping.\n";
    } else if (outcome == LoopSlotSelectionOutcome::Stopped) {
        std::cout << "Loop slot " << id.value() + 1 << " muted.\n";
    }

    return {
        .id = id.value(),
        .outcome = outcome,
    };
}

void LoopSlotGroup::cancelRecording(SlotId slot) {
    impl_->slot(slot).cancelRecording();
    impl_->pending_take.reset();
}

void LoopSlotGroup::completeRecording(
    SlotId slot,
    const TakeTiming& timing
) {
    const auto completion_offset = elapsedMilliseconds(
        timing.recording_started_at,
        timing.completed_at
    );
    impl_->pending_take.finish(completion_offset);
    impl_->slot(slot).recordingCompleted(
        impl_->pending_take.events(),
        impl_->pending_take.contentDuration(),
        timing
    );
    impl_->pending_take.reset();
}

void LoopSlotGroup::terminateAll() {
    for (SlotId id = 1; id < impl_->slots.size(); ++id) {
        impl_->slot(id).terminationRequested();
    }
    impl_->guide().terminationRequested();
}

void LoopSlotGroup::selectSoundFont(
    SlotId slot,
    const SoundFontDefinition& soundfont
) {
    impl_->slot(slot).selectSoundFont(soundfont);
}

void LoopSlotGroup::octaveDown(SlotId slot) {
    impl_->slot(slot).octaveDown();
}

void LoopSlotGroup::octaveUp(SlotId slot) {
    impl_->slot(slot).octaveUp();
}

void LoopSlotGroup::recordNote(
    SlotId slot,
    RecordedNoteKind kind,
    const MidiMessage& message,
    Milliseconds offset
) {
    const auto transposed = impl_->slot(slot).transpose(message);
    impl_->pending_take.record(kind, transposed, offset);
}

void LoopSlotGroup::stopDependentSlots() {
    for (SlotId id = 1; id < impl_->slots.size(); ++id) {
        impl_->slot(id).deactivate();
    }
}

} // namespace zeta
