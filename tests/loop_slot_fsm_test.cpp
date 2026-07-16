#include "../loop_slot_fsm.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <hegel/hegel.h>

#include <vector>

namespace {

namespace gs = hegel::generators;
using ::testing::InSequence;
using ::testing::StrictMock;
using zeta::SlotPlaybackFsm;
using zeta::SlotPlaybackOutput;
using zeta::SlotPlaybackState;

class MockOutput : public SlotPlaybackOutput {
public:
    MOCK_METHOD(void, activatePlayback, (), (override));
    MOCK_METHOD(void, deactivatePlayback, (), (override));
    MOCK_METHOD(void, terminatePlayback, (), (override));
};

TEST(SlotPlaybackFsmTest, ExplicitCommandsAreIdempotentAndTerminationIsFinal) {
    StrictMock<MockOutput> output;
    SlotPlaybackFsm fsm{output};

    EXPECT_EQ(fsm.state(), SlotPlaybackState::Muted);
    EXPECT_EQ(fsm.muteRequested(), SlotPlaybackState::Muted);

    EXPECT_CALL(output, activatePlayback());
    EXPECT_EQ(fsm.startRequested(), SlotPlaybackState::Looping);
    EXPECT_EQ(fsm.startRequested(), SlotPlaybackState::Looping);

    EXPECT_CALL(output, deactivatePlayback());
    EXPECT_EQ(fsm.muteRequested(), SlotPlaybackState::Muted);

    EXPECT_CALL(output, terminatePlayback());
    EXPECT_EQ(fsm.terminationRequested(), SlotPlaybackState::Terminated);
    EXPECT_EQ(fsm.startRequested(), SlotPlaybackState::Terminated);
    EXPECT_EQ(fsm.muteRequested(), SlotPlaybackState::Terminated);
    EXPECT_EQ(fsm.terminationRequested(), SlotPlaybackState::Terminated);
}

class CountingOutput final : public SlotPlaybackOutput {
public:
    void activatePlayback() override { ++activations; }
    void deactivatePlayback() override { ++deactivations; }
    void terminatePlayback() override { ++terminations; }

    int activations{};
    int deactivations{};
    int terminations{};
};

TEST(SlotPlaybackFsmProperty, EveryCommandSequenceMatchesTheTwoStateModel) {
    hegel::test([](hegel::TestCase& tc) {
        const auto commands = tc.draw(gs::vectors(
            gs::integers<int>({.min_value = 0, .max_value = 2}),
            {.max_size = 100}
        ));

        CountingOutput output;
        SlotPlaybackFsm fsm{output};
        SlotPlaybackState model = SlotPlaybackState::Muted;
        int expected_activations = 0;
        int expected_deactivations = 0;
        int expected_terminations = 0;

        for (const int command : commands) {
            if (command == 0) {
                if (model == SlotPlaybackState::Muted) {
                    model = SlotPlaybackState::Looping;
                    ++expected_activations;
                }
                EXPECT_EQ(fsm.startRequested(), model);
            } else if (command == 1) {
                if (model == SlotPlaybackState::Looping) {
                    model = SlotPlaybackState::Muted;
                    ++expected_deactivations;
                }
                EXPECT_EQ(fsm.muteRequested(), model);
            } else {
                if (model != SlotPlaybackState::Terminated) {
                    model = SlotPlaybackState::Terminated;
                    ++expected_terminations;
                }
                EXPECT_EQ(fsm.terminationRequested(), model);
            }

            EXPECT_EQ(fsm.state(), model);
            EXPECT_EQ(output.activations, expected_activations);
            EXPECT_EQ(output.deactivations, expected_deactivations);
            EXPECT_EQ(output.terminations, expected_terminations);
        }
    });
}

} // namespace
