#pragma once

#include "configuration.hpp"
#include "loop_slot.hpp"
#include "looper_fsm.hpp"

#include <memory>
#include <vector>

namespace zeta {

class SynthEngine;

class LoopSlotGroup final : private LoopSlotGroupOutput {
public:
    LoopSlotGroup(
        const std::vector<LoopSlotDefinition>& definitions,
        SynthEngine& synth_engine
    );
    ~LoopSlotGroup();

    LoopSlotGroup(const LoopSlotGroup&) = delete;
    LoopSlotGroup& operator=(const LoopSlotGroup&) = delete;

    int channel(SlotId slot) const;
    int monitorMidi(SlotId slot, const MidiMessage& message);

    LoopSlotSelectionResult requestSelection(
        int key,
        const SoundFontDefinition& soundfont,
        const OctaveTransposer& transposer
    );
    void cancelRecording(SlotId slot);
    void completeRecording(SlotId slot, const TakeTiming& timing);
    void terminateAll();

    void selectSoundFont(
        SlotId slot,
        const SoundFontDefinition& soundfont
    );
    void octaveDown(SlotId slot);
    void octaveUp(SlotId slot);

    void recordNote(
        SlotId slot,
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    );

private:
    struct Impl;

    void stopDependentSlots() override;

    std::unique_ptr<Impl> impl_;
};

} // namespace zeta
