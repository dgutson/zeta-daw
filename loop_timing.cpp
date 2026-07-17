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

TimePoint firstMatchingGuidePhaseAtOrAfter(
    const TakeTiming& timing,
    const LoopPlaybackSchedule& guide
) {
    const auto clock_period =
        std::chrono::duration_cast<LooperClock::duration>(guide.period);
    auto recorded_phase =
        (timing.recording_started_at - guide.first_cycle_at) % clock_period;
    if (recorded_phase < LooperClock::duration::zero()) {
        recorded_phase += clock_period;
    }

    const auto first_phase_at_origin = guide.first_cycle_at + recorded_phase;
    if (timing.completed_at <= first_phase_at_origin) {
        return first_phase_at_origin;
    }

    const auto elapsed = timing.completed_at - first_phase_at_origin;
    auto period_count = elapsed / clock_period;
    auto first_cycle_at = first_phase_at_origin + clock_period * period_count;
    if (first_cycle_at < timing.completed_at) {
        ++period_count;
        first_cycle_at = first_phase_at_origin + clock_period * period_count;
    }
    return first_cycle_at;
}

} // namespace

LoopPlaybackSchedule LoopPlaybackSchedule::forGuide(
    const TakeTiming& timing
) {
    return {
        .first_cycle_at = timing.completed_at,
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

    return {
        .first_cycle_at = firstMatchingGuidePhaseAtOrAfter(timing, guide),
        .period = smallestGuideMultiple(content_duration, guide.period),
    };
}

} // namespace zeta
