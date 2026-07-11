#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>

namespace zeta {

using LooperClock = std::chrono::steady_clock;
using TimePoint = LooperClock::time_point;
using Milliseconds = std::chrono::milliseconds;

enum class MidiMessageType : int {
    Other = -1,
    NoteOff = 0x80,
    NoteOn = 0x90,
    ControlChange = 0xB0,
    ProgramChange = 0xC0,
    PitchBend = 0xE0,
};

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

struct MidiMessage {
    void* native_event{};
    int raw_type{};
    int channel{};
    int key{};
    int velocity{};
    int control{};
    int value{};
    int program{};
    int pitch{};
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

    virtual int monitorMidi(MidiMessage& message, MidiRoute route) = 0;

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
    virtual ~LooperState() = default;

    virtual StateId primaryControlPressed(
        LooperStateData& data,
        LooperOutput& output,
        TimePoint now
    ) const = 0;

    virtual MidiHandlingResult midiMessage(
        LooperStateData& data,
        LooperOutput& output,
        MidiMessageType type,
        MidiMessage& message,
        TimePoint received_at
    ) const = 0;

    virtual StateId shutdownRequested(
        LooperStateData& data,
        LooperOutput& output
    ) const = 0;
};

class LooperStateRegistry {
public:
    LooperStateRegistry();
    ~LooperStateRegistry();

    LooperStateRegistry(const LooperStateRegistry&) = delete;
    LooperStateRegistry& operator=(const LooperStateRegistry&) = delete;

    const LooperState& at(StateId id) const;

private:
    static constexpr std::size_t state_count =
        static_cast<std::size_t>(StateId::Count);

    std::array<std::unique_ptr<LooperState>, state_count> states_;
};

class LooperFsm {
public:
    LooperFsm(LooperStateRegistry& states, LooperOutput& output) noexcept;

    LooperFsm(const LooperFsm&) = delete;
    LooperFsm& operator=(const LooperFsm&) = delete;

    StateId primaryControlPressed(TimePoint now);
    int midiMessage(MidiMessageType type, MidiMessage message, TimePoint received_at);
    StateId shutdownRequested();

    bool shouldRun() const;
    StateId stateId() const;

private:
    void install(StateId next_state);

    LooperStateRegistry& states_;
    LooperOutput& output_;
    LooperStateData data_;
    StateId current_state_{StateId::Ready};
    mutable std::mutex mutex_;
};

MidiMessageType classifyMidiMessage(int raw_type) noexcept;
bool isTerminal(StateId id) noexcept;

} // namespace zeta
