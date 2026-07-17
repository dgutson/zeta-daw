#pragma once

#include "configuration.hpp"
#include "loop_slot_fsm.hpp"
#include "loop_timing.hpp"
#include "looper_fsm.hpp"
#include "octave_transposer.hpp"
#include "pending_take.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace zeta {

class SynthEngine;

struct LoopSlotSelectionContext {
    const SoundFontDefinition& soundfont;
    const OctaveTransposer& transposer;
    std::optional<LoopPlaybackSchedule> guide_schedule;
};

class LoopSlotGroupOutput {
public:
    virtual ~LoopSlotGroupOutput() = default;
    virtual void stopDependentSlots() = 0;
};

class LoopSlot : private LoopSlotPlaybackOutput {
public:
    virtual ~LoopSlot();

    LoopSlot(const LoopSlot&) = delete;
    LoopSlot& operator=(const LoopSlot&) = delete;

    SlotId id() const noexcept;
    int selectionKey() const noexcept;
    int channel() const noexcept;
    std::optional<LoopPlaybackSchedule> activeSchedule() const;

    LoopSlotSelectionOutcome selectionRequested(
        const LoopSlotSelectionContext& context
    );
    void cancelRecording();
    void recordingCompleted(
        const std::vector<RecordedLoopEvent>& events,
        Milliseconds content_duration,
        const TakeTiming& timing
    );

    void selectSoundFont(const SoundFontDefinition& soundfont);
    void octaveDown();
    void octaveUp();
    MidiMessage transpose(const MidiMessage& message) const;
    int monitorMidi(const MidiMessage& message);

    void deactivate();
    void terminationRequested();

protected:
    LoopSlot(
        SlotId id,
        const LoopSlotDefinition& definition,
        SynthEngine& synth_engine
    );

    virtual LoopSlotSelectionOutcome onMutedSelection(
        const LoopSlotSelectionContext& context
    ) = 0;
    virtual void onLoopingSelection() = 0;
    virtual LoopPlaybackSchedule makeSchedule(
        const TakeTiming& timing,
        Milliseconds content_duration,
        const std::optional<LoopPlaybackSchedule>& guide
    ) const = 0;

    LoopSlotSelectionOutcome arm(
        const LoopSlotSelectionContext& context,
        std::optional<LoopPlaybackSchedule> guide
    );

private:
    struct PlaybackTake {
        std::vector<RecordedLoopEvent> events;
        LoopPlaybackSchedule schedule;
    };

    static bool isPlayablePeriod(Milliseconds period) noexcept;
    LoopSlotPlaybackState playbackState() const;

    void activatePlayback() override;
    void deactivatePlayback() override;
    void terminatePlayback() override;

    void workerMain(std::stop_token stop_token);
    void playRecordedEvent(const RecordedLoopEvent& event);
    void invalidateAndSilence();

    SlotId id_;
    int selection_key_;
    int channel_;
    SynthEngine& synth_engine_;
    const SoundFontDefinition* soundfont_{};
    OctaveTransposer transposer_;
    std::optional<LoopPlaybackSchedule> prepared_guide_;

    mutable std::mutex command_mutex_;
    LoopSlotPlaybackFsm playback_fsm_;

    mutable std::mutex playback_mutex_;
    std::condition_variable playback_changed_;
    std::shared_ptr<const PlaybackTake> committed_take_;
    std::uint64_t playback_generation_{};
    std::jthread worker_;
};

class GuideLoopSlot final : public LoopSlot {
public:
    GuideLoopSlot(
        SlotId id,
        const LoopSlotDefinition& definition,
        SynthEngine& synth_engine,
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

class RegularLoopSlot final : public LoopSlot {
public:
    RegularLoopSlot(
        SlotId id,
        const LoopSlotDefinition& definition,
        SynthEngine& synth_engine
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
