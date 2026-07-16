#pragma once

#include "loop_slot.hpp"
#include "midi_event.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>

namespace zeta {

using LooperClock = std::chrono::steady_clock;
using TimePoint = LooperClock::time_point;
using Milliseconds = std::chrono::milliseconds;

enum class MidiRouteKind {
    Live,
    RecordingSlot,
};

struct MidiRoute {
    MidiRouteKind kind{MidiRouteKind::Live};
    SlotId slot{};

    static MidiRoute live() noexcept;
    static MidiRoute recordingSlot(SlotId slot) noexcept;

    bool operator==(const MidiRoute&) const = default;
};

enum class StateId : std::size_t {
    Ready,
    ReadySelectingSoundFont,
    ReadySelectingLoopSlot,
    Armed,
    ArmedSelectingSoundFont,
    ArmedSelectingLoopSlot,
    Recording,
    RecordingSelectingLoopSlot,
    Stopped,
    Count,
};

struct MidiHandlingResult {
    StateId next_state{StateId::Ready};
    int native_result{};
};

struct LooperStateData {
    std::optional<SlotId> recording_slot;
    std::optional<int> selection_release_to_suppress;
    TimePoint recording_started_at{};
};

class LoopSlotView {
public:
    virtual ~LoopSlotView() = default;

    virtual std::optional<SlotId> slotByKey(int key) const = 0;
    virtual bool slotHasTake(SlotId slot) const = 0;
    virtual SlotPlaybackState slotPlaybackState(SlotId slot) const = 0;
};

class LooperOutput {
public:
    virtual ~LooperOutput() = default;

    virtual int monitorMidi(const MidiMessage& message, MidiRoute route) = 0;
    virtual void selectCurrentSoundFont(MidiRoute route) = 0;
    virtual void selectNextSoundFont(MidiRoute route) = 0;
    virtual void selectSoundFontByNote(MidiRoute route, int key) = 0;
    virtual void octaveDown(MidiRoute route) = 0;
    virtual void octaveUp(MidiRoute route) = 0;

    virtual void prepareTake(SlotId slot) = 0;
    virtual void discardPendingTake(SlotId slot) = 0;
    virtual void recordNote(
        SlotId slot,
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    ) = 0;
    virtual void commitTake(SlotId slot, Milliseconds duration) = 0;

    virtual void startSlotPlayback(SlotId slot) = 0;
    virtual void muteSlotPlayback(SlotId slot) = 0;
    virtual void terminateSlots() = 0;
    virtual void silenceAllChannels() = 0;

    virtual void showRecordingArmed(SlotId slot) = 0;
    virtual void showLooping(SlotId slot) = 0;
    virtual void showMuted(SlotId slot) = 0;
    virtual void showNoTake(SlotId slot) = 0;
    virtual void showRecorderBusy(SlotId slot) = 0;
    virtual void showUnknownLoopSlot(int key) = 0;
};

class LooperState {
public:
    LooperState(
        LooperOutput& output,
        const LoopSlotView& slots,
        LooperStateData& data
    ) noexcept;
    virtual ~LooperState() = default;

    virtual StateId loopSlotControlPressed() const = 0;
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
    LooperOutput& output_;
    const LoopSlotView& slots_;
    LooperStateData& data_;
};

class LooperStateRegistry {
public:
    LooperStateRegistry(LooperOutput& output, const LoopSlotView& slots);
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

    StateId loopSlotControlPressed();
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
