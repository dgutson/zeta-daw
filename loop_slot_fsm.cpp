#include "loop_slot_fsm.hpp"

namespace zeta {

SlotPlaybackFsm::SlotPlaybackFsm(SlotPlaybackOutput& output) noexcept
    : output_(output) {}

SlotPlaybackState SlotPlaybackFsm::startRequested() {
    std::lock_guard lock(mutex_);
    if (state_ == SlotPlaybackState::Muted) {
        output_.activatePlayback();
        state_ = SlotPlaybackState::Looping;
    }
    return state_;
}

SlotPlaybackState SlotPlaybackFsm::muteRequested() {
    std::lock_guard lock(mutex_);
    if (state_ == SlotPlaybackState::Looping) {
        output_.deactivatePlayback();
        state_ = SlotPlaybackState::Muted;
    }
    return state_;
}

SlotPlaybackState SlotPlaybackFsm::terminationRequested() {
    std::lock_guard lock(mutex_);
    if (state_ != SlotPlaybackState::Terminated) {
        output_.terminatePlayback();
        state_ = SlotPlaybackState::Terminated;
    }
    return state_;
}

SlotPlaybackState SlotPlaybackFsm::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

} // namespace zeta
