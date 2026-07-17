#include "../pending_take.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>

namespace {

using namespace std::chrono_literals;

using zeta::MidiMessage;
using zeta::PendingTake;
using zeta::RecordedNoteKind;

MidiMessage note(int key, int velocity = 100) {
    return {
        .key = key,
        .velocity = velocity,
    };
}

TEST(PendingTakeTest, ReleasedNoteDiscardsTrailingCompletionDelay) {
    PendingTake take;
    take.record(RecordedNoteKind::NoteOn, note(60), 0ms);
    take.record(RecordedNoteKind::NoteOff, note(60, 0), 10ms);

    take.finish(30ms);

    ASSERT_EQ(take.events().size(), 2U);
    EXPECT_EQ(take.contentDuration(), 10ms);
    EXPECT_EQ(take.events().back().kind, RecordedNoteKind::NoteOff);
    EXPECT_EQ(take.events().back().time_ms, 10U);
}

TEST(PendingTakeTest, CompletionClosesEveryHeldRecordedKey) {
    PendingTake take;
    take.record(RecordedNoteKind::NoteOn, note(60), 0ms);
    take.record(RecordedNoteKind::NoteOn, note(64), 2ms);
    take.record(RecordedNoteKind::NoteOff, note(60, 0), 5ms);

    take.finish(20ms);

    ASSERT_EQ(take.events().size(), 4U);
    EXPECT_EQ(take.contentDuration(), 20ms);
    EXPECT_EQ(take.events().back().key, 64);
    EXPECT_EQ(take.events().back().kind, RecordedNoteKind::NoteOff);
    EXPECT_EQ(take.events().back().time_ms, 20U);
}

TEST(PendingTakeTest, IgnoresReleaseWithoutAcceptedNoteOn) {
    PendingTake take;

    take.record(RecordedNoteKind::NoteOff, note(60, 0), 12ms);
    take.finish(30ms);

    EXPECT_TRUE(take.events().empty());
    EXPECT_EQ(take.contentDuration(), 0ms);
}

TEST(PendingTakeTest, FixedCapacityAlwaysReservesHeldNoteRelease) {
    constexpr std::size_t maximum_event_count = 16384;
    PendingTake take;

    for (std::size_t pair = 0; pair < maximum_event_count / 2 - 1; ++pair) {
        take.record(RecordedNoteKind::NoteOn, note(60), 0ms);
        take.record(RecordedNoteKind::NoteOff, note(60, 0), 1ms);
    }
    take.record(RecordedNoteKind::NoteOn, note(64), 2ms);
    take.record(RecordedNoteKind::NoteOn, note(67), 3ms);
    take.finish(4ms);

    ASSERT_EQ(take.events().size(), maximum_event_count);
    EXPECT_EQ(take.events().back().key, 64);
    EXPECT_EQ(take.events().back().kind, RecordedNoteKind::NoteOff);
    EXPECT_EQ(take.events().back().time_ms, 4U);
}

TEST(PendingTakeTest, ResetRetainsReusableEmptyTakeSemantics) {
    PendingTake take;
    take.record(RecordedNoteKind::NoteOn, note(60), 0ms);
    take.finish(5ms);

    take.reset();

    EXPECT_TRUE(take.events().empty());
    EXPECT_EQ(take.contentDuration(), 0ms);
}

} // namespace
