#pragma once

#include <mutex>

namespace zeta {

enum class SlotPlaybackState {
    Muted,
    Looping,
    Terminated,
};

class SlotPlaybackOutput {
public:
    virtual ~SlotPlaybackOutput() = default;

    virtual void activatePlayback() = 0;
    virtual void deactivatePlayback() = 0;
    virtual void terminatePlayback() = 0;
};

class SlotPlaybackFsm final {
public:
    explicit SlotPlaybackFsm(SlotPlaybackOutput& output) noexcept;

    SlotPlaybackFsm(const SlotPlaybackFsm&) = delete;
    SlotPlaybackFsm& operator=(const SlotPlaybackFsm&) = delete;

    SlotPlaybackState startRequested();
    SlotPlaybackState muteRequested();
    SlotPlaybackState terminationRequested();

    SlotPlaybackState state() const;

private:
    SlotPlaybackOutput& output_;
    SlotPlaybackState state_{SlotPlaybackState::Muted};
    mutable std::mutex mutex_;
};

} // namespace zeta
