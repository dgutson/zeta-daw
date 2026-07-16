#pragma once

#include "configuration.hpp"
#include "looper_fsm.hpp"
#include "midi_input.hpp"

#include <atomic>
#include <memory>
#include <string>

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

    static bool isPlayableLoopDuration(Milliseconds duration) noexcept;

    void handleMidiEvent(MidiEvent event) noexcept;

    int monitorMidi(const MidiMessage& message, MidiRoute route) override;
    void selectCurrentSoundFont(MidiRoute route) override;
    void selectNextSoundFont(MidiRoute route) override;
    void selectSoundFontByNote(
        MidiRoute route,
        int key
    ) override;
    void octaveDown(MidiRoute route) override;
    void octaveUp(MidiRoute route) override;

    void stopLoopPlayback() override;
    void silenceAllChannels() override;

    void resetTake() override;
    void recordNote(
        RecordedNoteKind kind,
        const MidiMessage& message,
        Milliseconds offset
    ) override;
    void commitTake(Milliseconds duration) override;
    void startLoopPlayback() override;

    void showRecordingArmed() override;
    void showLooping() override;
    void showNoTake() override;

    void stopPlaybackWorker() override;
};

} // namespace zeta
