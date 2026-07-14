#pragma once

#include "midi_event.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>

namespace zeta {

using LooperClock = std::chrono::steady_clock;
using TimePoint = LooperClock::time_point;
using Milliseconds = std::chrono::milliseconds;

enum class MidiRoute {
    LiveChannel,
    LoopChannel,
};

enum class RecordedNoteKind {
    NoteOn,
    NoteOff,
};

enum class StateId : std::size_t {
    Ready,
    Armed,
    Recording,
    Looping,
    Stopped,
    Count,
};

struct MidiHandlingResult {
    StateId next_state{StateId::Ready};
    int native_result{};
};

struct LooperStateData {
    TimePoint recording_started_at{};
};

class LooperOutput {
public:
    virtual ~LooperOutput() = default;

    virtual int monitorMidi(const MidiMessage& message, MidiRoute route) = 0;
    virtual void selectCurrentSoundFont(MidiRoute route) = 0;
    virtual void selectNextSoundFont(MidiRoute route) = 0;
    virtual void octaveDown(MidiRoute route) = 0;
    virtual void octaveUp(MidiRoute route) = 0;

    virtual void stopLoopPlayback() = 0;
    virtual void silenceAllChannels() = 0;

    virtual void resetTake() = 0;
    virtual void recordNote(
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    ) = 0;
    virtual void commitTake(Milliseconds duration) = 0;
    virtual void startLoopPlayback() = 0;

    virtual void showRecordingArmed() = 0;
    virtual void showLooping() = 0;
    virtual void showNoTake() = 0;

    virtual void stopPlaybackWorker() = 0;
};

class LooperState {
public:
    LooperState(LooperOutput& output, LooperStateData& data) noexcept;
    virtual ~LooperState() = default;

    virtual StateId primaryControlPressed(TimePoint now) const = 0;

    virtual StateId nextSoundFontPressed() const = 0;

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

    StateId primaryControlPressed(TimePoint now);
    StateId nextSoundFontPressed();
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

bool isTerminal(StateId id) noexcept;

} // namespace zeta
