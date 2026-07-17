#pragma once

#include "loop_timing.hpp"
#include "midi_event.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>

namespace zeta {

using SlotId = std::size_t;
enum class RecordedNoteKind {
    NoteOn,
    NoteOff,
};

enum class StateId : std::size_t {
    Ready,
    ReadySelectingLoopSlot,
    ReadySelectingSoundFont,
    Armed,
    ArmedSelectingSoundFont,
    Recording,
    Stopped,
    Count,
};

enum class LoopSlotSelectionOutcome {
    Armed,
    Stopped,
    GuideRequired,
    Unavailable,
};

struct LoopSlotSelectionResult {
    SlotId id{};
    LoopSlotSelectionOutcome outcome{LoopSlotSelectionOutcome::Unavailable};
};

struct MidiHandlingResult {
    StateId next_state{StateId::Ready};
    int native_result{};
};

struct LooperStateData {
    std::optional<SlotId> selected_recording_slot;
    TimePoint recording_started_at{};
};

class LooperOutput {
public:
    virtual ~LooperOutput() = default;

    virtual int monitorLiveMidi(const MidiMessage& message) = 0;
    virtual int monitorLoopSlotMidi(
        SlotId slot,
        const MidiMessage& message
    ) = 0;

    virtual LoopSlotSelectionResult requestLoopSlotSelection(int key) = 0;
    virtual void cancelLoopSlotRecording(SlotId slot) = 0;
    virtual void completeLoopSlotRecording(
        SlotId slot,
        const TakeTiming& timing
    ) = 0;
    virtual void terminateLoopSlots() = 0;

    virtual void selectCurrentLiveSoundFont() = 0;
    virtual void selectNextLiveSoundFont() = 0;
    virtual void selectNextLoopSlotSoundFont(SlotId slot) = 0;
    virtual void selectLiveSoundFontByNote(int key) = 0;
    virtual void selectLoopSlotSoundFontByNote(SlotId slot, int key) = 0;

    virtual void octaveDownLive() = 0;
    virtual void octaveUpLive() = 0;
    virtual void octaveDownLoopSlot(SlotId slot) = 0;
    virtual void octaveUpLoopSlot(SlotId slot) = 0;

    virtual void recordNote(
        SlotId slot,
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    ) = 0;
    virtual void showRecordingArmed(SlotId slot) = 0;
    virtual void showLooping(SlotId slot) = 0;
    virtual void showNoTake(SlotId slot) = 0;

    virtual void silenceAllChannels() = 0;
};

class LooperState {
public:
    LooperState(LooperOutput& output, LooperStateData& data) noexcept;
    virtual ~LooperState() = default;

    virtual StateId loopSlotControlPressed(TimePoint now) const = 0;
    virtual StateId nextSoundFontPressed() const = 0;
    virtual StateId soundFontByNotePressed() const = 0;
    virtual StateId octaveDownPressed() const = 0;
    virtual StateId octaveUpPressed() const = 0;

    virtual MidiHandlingResult midiMessage(
        MidiMessageType type,
        MidiMessage& message,
        TimePoint received_at
    ) const = 0;

    virtual StateId shutdownRequested() const = 0;

protected:
    SlotId selectedRecordingSlot() const;

    LooperOutput& output_;
    LooperStateData& data_;
};

class LooperStateRegistry {
public:
    explicit LooperStateRegistry(LooperOutput& output);
    ~LooperStateRegistry();

    LooperStateRegistry(const LooperStateRegistry&) = delete;
    LooperStateRegistry& operator=(const LooperStateRegistry&) = delete;

    const LooperState& at(StateId id) const;

private:
    static constexpr std::size_t state_count =
        static_cast<std::size_t>(StateId::Count);

    LooperStateData data_;
    std::array<std::unique_ptr<LooperState>, state_count> states_;
};

class LooperFsm {
public:
    explicit LooperFsm(LooperStateRegistry& states) noexcept;

    LooperFsm(const LooperFsm&) = delete;
    LooperFsm& operator=(const LooperFsm&) = delete;

    StateId loopSlotControlPressed(TimePoint now);
    StateId nextSoundFontPressed();
    StateId soundFontByNotePressed();
    StateId octaveDownPressed();
    StateId octaveUpPressed();
    int midiMessage(MidiMessageType type, MidiMessage message, TimePoint received_at);
    StateId shutdownRequested();

    bool shouldRun() const;
    StateId stateId() const;

private:
    void install(StateId next_state);

    LooperStateRegistry& states_;
    StateId current_state_{StateId::Ready};
    mutable std::mutex mutex_;
};

} // namespace zeta
