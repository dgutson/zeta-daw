#pragma once

#include "configuration.hpp"
#include "looper_fsm.hpp"
#include "midi_input.hpp"

#include <atomic>
#include <memory>
#include <optional>

namespace zeta {

class Application final : private LooperOutput, private LoopSlotView {
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

    std::optional<SlotId> slotByKey(int key) const override;
    SlotPlaybackState slotPlaybackState(SlotId slot) const override;

    int monitorMidi(const MidiMessage& message, MidiRoute route) override;
    void selectCurrentSoundFont(MidiRoute route) override;
    void selectNextSoundFont(MidiRoute route) override;
    void selectSoundFontByNote(MidiRoute route, int key) override;
    void octaveDown(MidiRoute route) override;
    void octaveUp(MidiRoute route) override;

    void prepareTake(SlotId slot) override;
    void discardPendingTake(SlotId slot) override;
    void recordNote(
        SlotId slot,
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    ) override;
    void commitTake(SlotId slot, Milliseconds duration) override;

    void startSlotPlayback(SlotId slot) override;
    void muteSlotPlayback(SlotId slot) override;
    void terminateSlots() override;
    void silenceAllChannels() override;

    void showRecordingArmed(SlotId slot) override;
    void showLooping(SlotId slot) override;
    void showStopped(SlotId slot) override;
    void showNoTake(SlotId slot) override;
    void showUnknownLoopSlot(int key) override;
};

} // namespace zeta
