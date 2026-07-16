#pragma once

namespace zeta {

enum class LoopSlotPlaybackState {
    Muted,
    Looping,
    Terminated,
};

class LoopSlotPlaybackOutput {
public:
    virtual ~LoopSlotPlaybackOutput() = default;

    virtual void activatePlayback() = 0;
    virtual void deactivatePlayback() = 0;
    virtual void terminatePlayback() = 0;
};

class LoopSlotPlaybackFsm final {
public:
    explicit LoopSlotPlaybackFsm(LoopSlotPlaybackOutput& output) noexcept;

    LoopSlotPlaybackState startRequested();
    LoopSlotPlaybackState muteRequested();
    LoopSlotPlaybackState terminationRequested();

    LoopSlotPlaybackState state() const noexcept;

private:
    LoopSlotPlaybackOutput& output_;
    LoopSlotPlaybackState state_{LoopSlotPlaybackState::Muted};
};

} // namespace zeta
