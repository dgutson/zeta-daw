#pragma once

#include "loop_slot_fsm.hpp"
#include "midi_event.hpp"
#include "octave_transposer.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace zeta {

class SoundFontDefinition;
class SynthEngine;

using SlotId = std::size_t;
using SlotDuration = std::chrono::milliseconds;

enum class RecordedNoteKind {
    NoteOn,
    NoteOff,
};

struct LoopSlotConfig {
    SlotId id{};
    int selection_key{};
    int synth_channel{};
};

class LoopSlot final : private SlotPlaybackOutput {
public:
    LoopSlot(LoopSlotConfig config, SynthEngine& synth);
    ~LoopSlot() override;

    LoopSlot(const LoopSlot&) = delete;
    LoopSlot& operator=(const LoopSlot&) = delete;

    SlotId id() const noexcept;
    int selectionKey() const noexcept;
    int synthChannel() const noexcept;

    SlotPlaybackState playbackState() const;
    bool hasTake() const noexcept;

    void prepareTake(
        const SoundFontDefinition& soundfont,
        const OctaveTransposer& live_transposer
    );
    int monitorMidi(const MidiMessage& message);
    void octaveDown() noexcept;
    void octaveUp() noexcept;
    void selectSoundFont(const SoundFontDefinition& soundfont);
    void recordNote(
        RecordedNoteKind kind,
        const MidiMessage& message,
        SlotDuration offset
    );
    void commitTake(SlotDuration duration) noexcept;
    void discardPendingTake() noexcept;

    SlotPlaybackState startRequested();
    SlotPlaybackState muteRequested();
    SlotPlaybackState terminationRequested();

private:
    struct RecordedNoteEvent {
        std::uint64_t time_ms{};
        int key{};
        int velocity{};
        RecordedNoteKind kind{RecordedNoteKind::NoteOff};
    };

    static constexpr std::size_t max_recorded_events = 16384;

    SlotId id_;
    int selection_key_;
    int synth_channel_;
    SynthEngine& synth_;
    OctaveTransposer transposer_;

    std::vector<RecordedNoteEvent> events_;
    SlotDuration duration_{};
    bool has_take_{};

    std::jthread worker_;
    std::mutex playback_mutex_;
    std::condition_variable playback_changed_;
    bool playback_active_{};
    std::uint64_t playback_generation_{};

    SlotPlaybackFsm playback_fsm_;

    void activatePlayback() override;
    void deactivatePlayback() override;
    void terminatePlayback() override;

    void playbackMain(std::stop_token stop_token);
    void playRecordedEvent(const RecordedNoteEvent& event);
};

} // namespace zeta
