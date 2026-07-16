#pragma once

#include "configuration.hpp"
#include "looper_fsm.hpp"
#include "midi_input.hpp"

#include <atomic>
#include <memory>
#include <optional>

namespace zeta {

class Application final : private LooperOutput {
public:
    explicit Application(ApplicationConfig config);
    Application(ApplicationConfig config, std::unique_ptr<MidiInput> midi_input);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void run();
    void shutdownRequested();

private:
    struct Impl;

    ApplicationConfig config_;
    std::unique_ptr<Impl> impl_;
    LooperStateRegistry states_;
    LooperFsm fsm_;
    std::unique_ptr<MidiInput> midi_input_;
    std::atomic<bool> midi_ready_{false};

    void handleMidiEvent(MidiEvent event) noexcept;

    int monitorLiveMidi(const MidiMessage& message) override;
    int monitorLoopSlotMidi(
        SlotId slot,
        const MidiMessage& message
    ) override;

    std::optional<LoopSlotSelection> loopSlotByKey(int key) const override;
    void prepareLoopSlot(SlotId slot) override;
    void cancelLoopSlotRecording(SlotId slot) override;
    void muteLoopSlot(SlotId slot) override;
    void startLoopSlot(SlotId slot) override;
    void terminateLoopSlots() override;

    void selectCurrentLiveSoundFont() override;
    void selectNextLiveSoundFont() override;
    void selectNextLoopSlotSoundFont(SlotId slot) override;
    void selectLiveSoundFontByNote(int key) override;
    void selectLoopSlotSoundFontByNote(SlotId slot, int key) override;

    void octaveDownLive() override;
    void octaveUpLive() override;
    void octaveDownLoopSlot(SlotId slot) override;
    void octaveUpLoopSlot(SlotId slot) override;

    void resetPendingTake() override;
    void recordNote(
        SlotId slot,
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    ) override;
    void commitTake(SlotId slot, Milliseconds duration) override;

    void showRecordingArmed(SlotId slot) override;
    void showLooping(SlotId slot) override;
    void showNoTake(SlotId slot) override;

    void silenceAllChannels() override;
};

} // namespace zeta
