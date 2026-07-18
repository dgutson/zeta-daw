#include "../loop_timing.hpp"

#include <gtest/gtest.h>
#include <hegel/hegel.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

namespace gs = hegel::generators;
using namespace std::chrono_literals;

using zeta::LoopPlaybackSchedule;
using zeta::LooperClock;
using zeta::Milliseconds;
using zeta::TakeTiming;
using zeta::TimePoint;

struct GeneratedTiming {
    LoopPlaybackSchedule guide;
    TakeTiming take;
    Milliseconds content_duration;
};

GeneratedTiming generatedTiming(hegel::TestCase& tc) {
    const auto guide_period_count = tc.draw(
        gs::integers<std::uint32_t>({
            .min_value = 1,
            .max_value = std::numeric_limits<std::uint32_t>::max(),
        })
    );
    const auto guide_period = Milliseconds(guide_period_count);
    const auto guide_origin_count = tc.draw(gs::integers<std::uint32_t>());
    const auto guide_origin = TimePoint(Milliseconds(guide_origin_count));
    // Keeps even the maximum generated period inside steady_clock's range.
    constexpr std::uint16_t maximum_safe_elapsed_cycles = 1024;
    const auto elapsed_guide_cycles = tc.draw(
        gs::integers<std::uint16_t>({
            .min_value = 0,
            .max_value = maximum_safe_elapsed_cycles,
        })
    );
    const auto phase_count = tc.draw(gs::integers<std::uint32_t>({
        .min_value = 0,
        .max_value = guide_period_count - 1,
    }));
    const auto recording_started_at = guide_origin
        + guide_period * elapsed_guide_cycles
        + Milliseconds(phase_count);
    const auto completion_delay = tc.draw(gs::integers<std::uint32_t>());
    const auto completion_microseconds = tc.draw(
        gs::integers<std::uint16_t>({.min_value = 0, .max_value = 999})
    );

    return {
        .guide = {
            .first_cycle_at = guide_origin,
            .first_cycle_join_at = guide_origin,
            .period = guide_period,
        },
        .take = {
            .recording_started_at = recording_started_at,
            .completed_at = recording_started_at
                + Milliseconds(completion_delay)
                + std::chrono::microseconds(completion_microseconds),
        },
        .content_duration = Milliseconds(
            tc.draw(gs::integers<std::uint32_t>())
        ),
    };
}

HEGEL_TEST(regular_period_is_smallest_covering_guide_multiple)(
    hegel::TestCase& tc
) {
    const auto generated = generatedTiming(tc);
    const auto schedule = LoopPlaybackSchedule::forRegular(
        generated.take,
        generated.content_duration,
        generated.guide
    );

    const auto previous_multiple = schedule.period - generated.guide.period;
    const bool is_multiple =
        schedule.period.count() % generated.guide.period.count() == 0;
    const bool covers_content = schedule.period >= generated.content_duration;
    const bool is_smallest = generated.content_duration == Milliseconds::zero()
        ? schedule.period == generated.guide.period
        : previous_multiple < generated.content_duration;
    if (!is_multiple || !covers_content || !is_smallest) {
        throw std::runtime_error(
            "regular period is not the smallest covering guide multiple"
        );
    }
}

HEGEL_TEST(regular_first_cycle_joins_natural_repetition_at_completion)(
    hegel::TestCase& tc
) {
    const auto generated = generatedTiming(tc);
    const auto schedule = LoopPlaybackSchedule::forRegular(
        generated.take,
        generated.content_duration,
        generated.guide
    );
    const auto clock_period = std::chrono::duration_cast<LooperClock::duration>(
        schedule.period
    );
    const auto first_natural_cycle =
        generated.take.recording_started_at + clock_period;
    const auto cycle_offset =
        schedule.first_cycle_at - generated.take.recording_started_at;
    const bool is_natural_repetition =
        schedule.first_cycle_at >= first_natural_cycle
        && cycle_offset % clock_period == LooperClock::duration::zero();
    if (!is_natural_repetition) {
        throw std::runtime_error(
            "regular first cycle is not on the natural repetition timeline"
        );
    }

    if (generated.take.completed_at < first_natural_cycle) {
        if (schedule.first_cycle_at != first_natural_cycle
            || schedule.first_cycle_join_at != first_natural_cycle) {
            throw std::runtime_error(
                "regular first cycle did not wait for its first repetition"
            );
        }
        return;
    }

    const auto next_cycle_at = schedule.first_cycle_at + clock_period;
    if (schedule.first_cycle_at > generated.take.completed_at
        || next_cycle_at <= generated.take.completed_at
        || schedule.first_cycle_join_at != generated.take.completed_at) {
        throw std::runtime_error(
            "regular first cycle did not join the repetition at completion"
        );
    }
}

HEGEL_TEST(regular_first_cycle_preserves_recorded_guide_phase)(
    hegel::TestCase& tc
) {
    const auto generated = generatedTiming(tc);
    const auto schedule = LoopPlaybackSchedule::forRegular(
        generated.take,
        generated.content_duration,
        generated.guide
    );
    const auto clock_period = std::chrono::duration_cast<LooperClock::duration>(
        generated.guide.period
    );
    const auto recorded_phase =
        (generated.take.recording_started_at - generated.guide.first_cycle_at)
        % clock_period;
    const auto playback_phase =
        (schedule.first_cycle_at - generated.guide.first_cycle_at)
        % clock_period;

    if (recorded_phase != playback_phase) {
        throw std::runtime_error("regular playback changed the recorded phase");
    }
}

TEST(LoopTimingTest, GuideStartsAtCompletionWithRecordedDuration) {
    const TakeTiming timing{
        .recording_started_at = TimePoint(100ms),
        .completed_at = TimePoint(4100ms),
    };

    const auto schedule = LoopPlaybackSchedule::forGuide(timing);

    EXPECT_EQ(schedule.first_cycle_at, TimePoint(4100ms));
    EXPECT_EQ(schedule.first_cycle_join_at, TimePoint(4100ms));
    EXPECT_EQ(schedule.period, 4000ms);
}

TEST(LoopTimingTest, RegularStartsAtNextCapturedPhase) {
    const LoopPlaybackSchedule guide{
        .first_cycle_at = TimePoint(4000ms),
        .first_cycle_join_at = TimePoint(4000ms),
        .period = 4000ms,
    };
    const TakeTiming timing{
        .recording_started_at = TimePoint(5000ms),
        .completed_at = TimePoint(7900ms),
    };

    const auto schedule = LoopPlaybackSchedule::forRegular(
        timing,
        2500ms,
        guide
    );

    EXPECT_EQ(schedule.first_cycle_at, TimePoint(9000ms));
    EXPECT_EQ(schedule.first_cycle_join_at, TimePoint(9000ms));
    EXPECT_EQ(schedule.period, 4000ms);
}

TEST(LoopTimingTest, RegularJoinsNaturalRepetitionAlreadyInProgress) {
    const LoopPlaybackSchedule guide{
        .first_cycle_at = TimePoint(0ms),
        .first_cycle_join_at = TimePoint(0ms),
        .period = 100ms,
    };
    const TakeTiming timing{
        .recording_started_at = TimePoint(10ms),
        .completed_at = TimePoint(115ms),
    };

    const auto schedule = LoopPlaybackSchedule::forRegular(
        timing,
        85ms,
        guide
    );

    EXPECT_EQ(schedule.first_cycle_at, TimePoint(110ms));
    EXPECT_EQ(schedule.first_cycle_join_at, TimePoint(115ms));
    EXPECT_EQ(schedule.period, 100ms);
}

TEST(LoopTimingTest, RegularWaitsForFirstNaturalRepetition) {
    const LoopPlaybackSchedule guide{
        .first_cycle_at = TimePoint(0ms),
        .first_cycle_join_at = TimePoint(0ms),
        .period = 100ms,
    };
    const TakeTiming timing{
        .recording_started_at = TimePoint(80ms),
        .completed_at = TimePoint(115ms),
    };

    const auto schedule = LoopPlaybackSchedule::forRegular(
        timing,
        15ms,
        guide
    );

    EXPECT_EQ(schedule.first_cycle_at, TimePoint(180ms));
    EXPECT_EQ(schedule.first_cycle_join_at, TimePoint(180ms));
    EXPECT_EQ(schedule.period, 100ms);
}

TEST(LoopTimingTest, RegularUsesEnoughCompleteGuideCyclesForLongPhrase) {
    const LoopPlaybackSchedule guide{
        .first_cycle_at = TimePoint(4000ms),
        .first_cycle_join_at = TimePoint(4000ms),
        .period = 4000ms,
    };
    const TakeTiming timing{
        .recording_started_at = TimePoint(5000ms),
        .completed_at = TimePoint(15000ms),
    };

    const auto schedule = LoopPlaybackSchedule::forRegular(
        timing,
        9000ms,
        guide
    );

    EXPECT_EQ(schedule.first_cycle_at, TimePoint(17000ms));
    EXPECT_EQ(schedule.first_cycle_join_at, TimePoint(17000ms));
    EXPECT_EQ(schedule.period, 12000ms);
}

TEST(LoopTimingTest, LongRegularJoinsOnlyAtItsWholePhrasePeriod) {
    const LoopPlaybackSchedule guide{
        .first_cycle_at = TimePoint(0ms),
        .first_cycle_join_at = TimePoint(0ms),
        .period = 100ms,
    };
    const TakeTiming timing{
        .recording_started_at = TimePoint(10ms),
        .completed_at = TimePoint(215ms),
    };

    const auto schedule = LoopPlaybackSchedule::forRegular(
        timing,
        185ms,
        guide
    );

    EXPECT_EQ(schedule.first_cycle_at, TimePoint(210ms));
    EXPECT_EQ(schedule.first_cycle_join_at, TimePoint(215ms));
    EXPECT_EQ(schedule.period, 200ms);
}

TEST(LoopTimingTest, ExactGuideBoundariesDoNotAddAnotherCycle) {
    const LoopPlaybackSchedule guide{
        .first_cycle_at = TimePoint(4000ms),
        .first_cycle_join_at = TimePoint(4000ms),
        .period = 4000ms,
    };
    const TakeTiming timing{
        .recording_started_at = TimePoint(5000ms),
        .completed_at = TimePoint(13000ms),
    };

    const auto schedule = LoopPlaybackSchedule::forRegular(
        timing,
        8000ms,
        guide
    );

    EXPECT_EQ(schedule.first_cycle_at, TimePoint(13000ms));
    EXPECT_EQ(schedule.first_cycle_join_at, TimePoint(13000ms));
    EXPECT_EQ(schedule.period, 8000ms);
}

TEST(LoopTimingPropertyTest, PeriodIsSmallestCoveringGuideMultiple) {
    regular_period_is_smallest_covering_guide_multiple();
}

TEST(LoopTimingPropertyTest, FirstCycleJoinsNaturalRepetitionAtCompletion) {
    regular_first_cycle_joins_natural_repetition_at_completion();
}

TEST(LoopTimingPropertyTest, FirstCyclePreservesRecordedGuidePhase) {
    regular_first_cycle_preserves_recorded_guide_phase();
}

} // namespace
