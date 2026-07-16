#include "loop_slot_fsm.hpp"

namespace zeta {

LoopSlotPlaybackFsm::LoopSlotPlaybackFsm(
    LoopSlotPlaybackOutput& output
) noexcept : output_(output) {}

LoopSlotPlaybackState LoopSlotPlaybackFsm::startRequested() {
    if (state_ == LoopSlotPlaybackState::Muted) {
        output_.activatePlayback();
        state_ = LoopSlotPlaybackState::Looping;
    }
    return state_;
}

LoopSlotPlaybackState LoopSlotPlaybackFsm::muteRequested() {
    if (state_ == LoopSlotPlaybackState::Looping) {
        output_.deactivatePlayback();
        state_ = LoopSlotPlaybackState::Muted;
    }
    return state_;
}

LoopSlotPlaybackState LoopSlotPlaybackFsm::terminationRequested() {
    if (state_ != LoopSlotPlaybackState::Terminated) {
        output_.terminatePlayback();
        state_ = LoopSlotPlaybackState::Terminated;
    }
    return state_;
}

LoopSlotPlaybackState LoopSlotPlaybackFsm::state() const noexcept {
    return state_;
}

} // namespace zeta
