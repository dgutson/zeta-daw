#include "loop_timing.hpp"

#include <stdexcept>

namespace zeta {

Milliseconds elapsedMilliseconds(TimePoint start, TimePoint finish) noexcept {
    const auto elapsed = std::chrono::duration_cast<Milliseconds>(finish - start);
    return elapsed < Milliseconds::zero() ? Milliseconds::zero() : elapsed;
}

namespace {

Milliseconds smallestGuideMultiple(
    Milliseconds content_duration,
    Milliseconds guide_period
) {
    if (guide_period <= Milliseconds::zero()) {
        throw std::invalid_argument("Guide period must be positive");
    }

    if (content_duration <= Milliseconds::zero()) {
        return guide_period;
    }

    const auto quotient = content_duration.count() / guide_period.count();
    const auto remainder = content_duration.count() % guide_period.count();
    const auto period_count = quotient + (remainder == 0 ? 0 : 1);
    return guide_period * period_count;
}

TimePoint naturalRegularCycleAtCompletion(
    const TakeTiming& timing,
    Milliseconds period
) {
    const auto clock_period = std::chrono::duration_cast<LooperClock::duration>(
        period
    );
    const auto first_natural_cycle = timing.recording_started_at + clock_period;
    if (timing.completed_at < first_natural_cycle) {
        return first_natural_cycle;
    }

    const auto elapsed = timing.completed_at - first_natural_cycle;
    const auto elapsed_periods = elapsed / clock_period;
    return first_natural_cycle + clock_period * elapsed_periods;
}

} // namespace

LoopPlaybackSchedule LoopPlaybackSchedule::forGuide(
    const TakeTiming& timing
) {
    return {
        .first_cycle_at = timing.completed_at,
        .first_cycle_join_at = timing.completed_at,
        .period = elapsedMilliseconds(
            timing.recording_started_at,
            timing.completed_at
        ),
    };
}

LoopPlaybackSchedule LoopPlaybackSchedule::forRegular(
    const TakeTiming& timing,
    Milliseconds content_duration,
    const LoopPlaybackSchedule& guide
) {
    if (guide.period <= Milliseconds::zero()) {
        throw std::invalid_argument(
            "Regular slot requires a positive guide period"
        );
    }

    const auto period = smallestGuideMultiple(content_duration, guide.period);
    const auto first_cycle_at = naturalRegularCycleAtCompletion(timing, period);
    const auto first_cycle_join_at = timing.completed_at < first_cycle_at
        ? first_cycle_at
        : timing.completed_at;

    return {
        .first_cycle_at = first_cycle_at,
        .first_cycle_join_at = first_cycle_join_at,
        .period = period,
    };
}

} // namespace zeta
