#include "../loop_slot_fsm.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <hegel/hegel.h>

#include <stdexcept>
#include <vector>

namespace {

using zeta::LoopSlotPlaybackFsm;
using zeta::LoopSlotPlaybackOutput;
using zeta::LoopSlotPlaybackState;

class MockPlaybackOutput final : public LoopSlotPlaybackOutput {
public:
    MOCK_METHOD(void, activatePlayback, (), (override));
    MOCK_METHOD(void, deactivatePlayback, (), (override));
    MOCK_METHOD(void, terminatePlayback, (), (override));
};

class CountingPlaybackOutput final : public LoopSlotPlaybackOutput {
public:
    void activatePlayback() override {
        ++activations;
    }

    void deactivatePlayback() override {
        ++deactivations;
    }

    void terminatePlayback() override {
        ++terminations;
    }

    int activations{};
    int deactivations{};
    int terminations{};
};

enum class PlaybackModelState {
    Muted,
    Looping,
    Terminated,
};

class PlaybackLifecycleMachine final
    : public hegel::stateful::StateMachine<PlaybackLifecycleMachine> {
public:
    PlaybackLifecycleMachine() : subject_{output_} {}

    std::vector<hegel::stateful::Rule<PlaybackLifecycleMachine>> rules() {
        using Rule = hegel::stateful::Rule<PlaybackLifecycleMachine>;

        return {
            Rule("start request", [](hegel::TestCase&, auto& machine) {
                machine.startRequested();
            }),
            Rule("mute request", [](hegel::TestCase&, auto& machine) {
                machine.muteRequested();
            }),
            Rule("termination request", [](hegel::TestCase&, auto& machine) {
                machine.terminationRequested();
            }),
        };
    }

    std::vector<hegel::stateful::Invariant<PlaybackLifecycleMachine>>
    invariants() {
        using Invariant =
            hegel::stateful::Invariant<PlaybackLifecycleMachine>;

        return {
            Invariant("subject state matches model", [](const auto& machine) {
                if (!machine.statesAgree()) {
                    throw std::runtime_error(
                        "playback state disagrees with lifecycle model"
                    );
                }
            }),
            Invariant("output calls match model", [](const auto& machine) {
                if (!machine.outputCallsAgree()) {
                    throw std::runtime_error(
                        "playback output calls disagree with lifecycle model"
                    );
                }
            }),
            Invariant("termination occurs at most once", [](const auto& machine) {
                if (machine.output_.terminations > 1
                    || machine.expected_terminations_ > 1) {
                    throw std::runtime_error(
                        "playback termination occurred more than once"
                    );
                }
            }),
            Invariant("terminated state is absorbing", [](const auto& machine) {
                if (machine.was_terminated_
                    && (machine.model_state_
                            != PlaybackModelState::Terminated
                        || machine.subject_.state()
                            != LoopSlotPlaybackState::Terminated)) {
                    throw std::runtime_error(
                        "terminated playback state accepted a transition"
                    );
                }
            }),
        };
    }

private:
    void startRequested() {
        if (model_state_ == PlaybackModelState::Muted) {
            model_state_ = PlaybackModelState::Looping;
            ++expected_activations_;
        }
        subject_.startRequested();
    }

    void muteRequested() {
        if (model_state_ == PlaybackModelState::Looping) {
            model_state_ = PlaybackModelState::Muted;
            ++expected_deactivations_;
        }
        subject_.muteRequested();
    }

    void terminationRequested() {
        if (model_state_ != PlaybackModelState::Terminated) {
            model_state_ = PlaybackModelState::Terminated;
            ++expected_terminations_;
            was_terminated_ = true;
        }
        subject_.terminationRequested();
    }

    bool statesAgree() const {
        switch (model_state_) {
        case PlaybackModelState::Muted:
            return subject_.state() == LoopSlotPlaybackState::Muted;
        case PlaybackModelState::Looping:
            return subject_.state() == LoopSlotPlaybackState::Looping;
        case PlaybackModelState::Terminated:
            return subject_.state() == LoopSlotPlaybackState::Terminated;
        }
        return false;
    }

    bool outputCallsAgree() const {
        return output_.activations == expected_activations_
            && output_.deactivations == expected_deactivations_
            && output_.terminations == expected_terminations_;
    }

    CountingPlaybackOutput output_;
    LoopSlotPlaybackFsm subject_;
    PlaybackModelState model_state_{PlaybackModelState::Muted};
    int expected_activations_{};
    int expected_deactivations_{};
    int expected_terminations_{};
    bool was_terminated_{};
};

HEGEL_TEST(loop_slot_playback_stateful_lifecycle_matches_independent_model)(
    hegel::TestCase& tc
) {
    PlaybackLifecycleMachine machine;
    hegel::stateful::run(machine, tc);
}

TEST(LoopSlotPlaybackFsmTest, StartsOnlyFromMuted) {
    MockPlaybackOutput output;
    LoopSlotPlaybackFsm fsm{output};

    EXPECT_CALL(output, activatePlayback()).Times(1);
    EXPECT_EQ(fsm.startRequested(), LoopSlotPlaybackState::Looping);
    EXPECT_EQ(fsm.startRequested(), LoopSlotPlaybackState::Looping);
}

TEST(LoopSlotPlaybackFsmTest, MutesOnlyFromLooping) {
    MockPlaybackOutput output;
    LoopSlotPlaybackFsm fsm{output};

    EXPECT_CALL(output, deactivatePlayback()).Times(0);
    EXPECT_EQ(fsm.muteRequested(), LoopSlotPlaybackState::Muted);
    testing::Mock::VerifyAndClearExpectations(&output);

    EXPECT_CALL(output, activatePlayback()).Times(1);
    EXPECT_EQ(fsm.startRequested(), LoopSlotPlaybackState::Looping);
    testing::Mock::VerifyAndClearExpectations(&output);

    EXPECT_CALL(output, deactivatePlayback()).Times(1);
    EXPECT_EQ(fsm.muteRequested(), LoopSlotPlaybackState::Muted);
}

TEST(LoopSlotPlaybackFsmTest, TerminatesExactlyOnceFromEveryLiveState) {
    for (const bool start_first : {false, true}) {
        MockPlaybackOutput output;
        LoopSlotPlaybackFsm fsm{output};

        if (start_first) {
            EXPECT_CALL(output, activatePlayback()).Times(1);
            fsm.startRequested();
            testing::Mock::VerifyAndClearExpectations(&output);
        }

        EXPECT_CALL(output, terminatePlayback()).Times(1);
        EXPECT_EQ(
            fsm.terminationRequested(),
            LoopSlotPlaybackState::Terminated
        );
        EXPECT_EQ(
            fsm.terminationRequested(),
            LoopSlotPlaybackState::Terminated
        );
        EXPECT_EQ(fsm.startRequested(), LoopSlotPlaybackState::Terminated);
        EXPECT_EQ(fsm.muteRequested(), LoopSlotPlaybackState::Terminated);
    }
}

TEST(LoopSlotPlaybackPropertyTest, CommandsMatchIndependentModel) {
    loop_slot_playback_stateful_lifecycle_matches_independent_model();
}

} // namespace
