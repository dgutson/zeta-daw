#include "../loop_slot.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <hegel/hegel.h>

#include <stdexcept>
#include <vector>

namespace {

namespace gs = hegel::generators;

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

enum class Command {
    Start,
    Mute,
    Terminate,
};

Command commandFrom(int value) {
    if (value == 0) {
        return Command::Start;
    }
    if (value == 1) {
        return Command::Mute;
    }
    return Command::Terminate;
}

HEGEL_TEST(loop_slot_playback_commands_match_independent_model)(
    hegel::TestCase& tc
) {
    const auto commands = tc.draw(gs::vectors(
        gs::integers<int>({.min_value = 0, .max_value = 2})
    ));

    CountingPlaybackOutput output;
    LoopSlotPlaybackFsm subject{output};
    LoopSlotPlaybackState model = LoopSlotPlaybackState::Muted;
    int expected_activations = 0;
    int expected_deactivations = 0;
    int expected_terminations = 0;

    for (const int value : commands) {
        switch (commandFrom(value)) {
        case Command::Start:
            if (model == LoopSlotPlaybackState::Muted) {
                model = LoopSlotPlaybackState::Looping;
                ++expected_activations;
            }
            subject.startRequested();
            break;
        case Command::Mute:
            if (model == LoopSlotPlaybackState::Looping) {
                model = LoopSlotPlaybackState::Muted;
                ++expected_deactivations;
            }
            subject.muteRequested();
            break;
        case Command::Terminate:
            if (model != LoopSlotPlaybackState::Terminated) {
                model = LoopSlotPlaybackState::Terminated;
                ++expected_terminations;
            }
            subject.terminationRequested();
            break;
        }

        if (subject.state() != model
            || output.activations != expected_activations
            || output.deactivations != expected_deactivations
            || output.terminations != expected_terminations) {
            throw std::runtime_error(
                "loop-slot playback FSM disagrees with command model"
            );
        }
    }
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
    loop_slot_playback_commands_match_independent_model();
}

} // namespace
