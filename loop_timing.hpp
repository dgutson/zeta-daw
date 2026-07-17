#pragma once

#include <chrono>

namespace zeta {

using LooperClock = std::chrono::steady_clock;
using TimePoint = LooperClock::time_point;
using Milliseconds = std::chrono::milliseconds;

Milliseconds elapsedMilliseconds(TimePoint start, TimePoint finish) noexcept;

struct TakeTiming {
    TimePoint recording_started_at{};
    TimePoint completed_at{};
};

struct LoopPlaybackSchedule {
    TimePoint first_cycle_at{};
    Milliseconds period{};

    static LoopPlaybackSchedule forGuide(const TakeTiming& timing);

    static LoopPlaybackSchedule forRegular(
        const TakeTiming& timing,
        Milliseconds content_duration,
        const LoopPlaybackSchedule& guide
    );
};

} // namespace zeta
