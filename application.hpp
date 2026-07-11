#pragma once

#include "configuration.hpp"
#include "looper_fsm.hpp"

#include <fluidsynth.h>

#include <memory>
#include <string>

namespace zeta {

class Application final : private LooperOutput {
public:
    explicit Application(ApplicationConfig config);
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

    static int midiCallback(void* data, fluid_midi_event_t* event) noexcept;
    int handleMidiEvent(fluid_midi_event_t* event);

    int monitorMidi(MidiMessage& message, MidiRoute route) override;

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
