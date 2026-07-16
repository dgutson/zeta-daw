#pragma once

#include "configuration.hpp"
#include "loop_slot_fsm.hpp"
#include "looper_fsm.hpp"
#include "octave_transposer.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace zeta {

class SynthEngine;

struct RecordedLoopEvent {
    std::uint64_t time_ms{};
    int key{};
    int velocity{};
    RecordedNoteKind kind{RecordedNoteKind::NoteOff};
};

class LoopSlot final : private LoopSlotPlaybackOutput {
public:
    LoopSlot(
        SlotId id,
        const LoopSlotDefinition& definition,
        SynthEngine& synth_engine
    );
    ~LoopSlot();

    LoopSlot(const LoopSlot&) = delete;
    LoopSlot& operator=(const LoopSlot&) = delete;

    SlotId id() const noexcept;
    int selectionKey() const noexcept;
    int channel() const noexcept;
    LoopSlotPlaybackState playbackState() const;

    void prepareRecording(
        const SoundFontDefinition& soundfont,
        const OctaveTransposer& transposer
    );
    void selectSoundFont(const SoundFontDefinition& soundfont);
    void octaveDown();
    void octaveUp();
    MidiMessage transpose(const MidiMessage& message) const;
    int monitorMidi(const MidiMessage& message);

    void commitTake(
        const std::vector<RecordedLoopEvent>& events,
        Milliseconds duration
    );
    void cancelRecording();

    void startRequested();
    void muteRequested();
    void terminationRequested();

private:
    struct PlaybackTake {
        std::vector<RecordedLoopEvent> events;
        Milliseconds duration{};
    };

    static bool isPlayableDuration(Milliseconds duration) noexcept;

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

    mutable std::mutex command_mutex_;
    LoopSlotPlaybackFsm playback_fsm_;

    std::mutex playback_mutex_;
    std::condition_variable playback_changed_;
    std::shared_ptr<const PlaybackTake> committed_take_;
    std::uint64_t playback_generation_{};
    std::jthread worker_;
};

} // namespace zeta
