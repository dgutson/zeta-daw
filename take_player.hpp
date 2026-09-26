#pragma once

#include "loop_timing.hpp"
#include "looper_fsm.hpp"
#include "pending_take.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace zeta {

class SynthEngine;

class TakePlayer final {
public:
    TakePlayer(
        SynthEngine& synth_engine,
        SlotId id,
        int channel
    );

    TakePlayer(const TakePlayer&) = delete;
    TakePlayer& operator=(const TakePlayer&) = delete;

    std::optional<LoopPlaybackSchedule> schedule() const;
    void commitTake(
        const std::vector<RecordedLoopEvent>& events,
        const LoopPlaybackSchedule& schedule
    );
    void invalidateAndSilence();

    void activatePlayback();
    void deactivatePlayback();
    void terminatePlayback();

private:
    struct PlaybackTake {
        std::vector<RecordedLoopEvent> events;
        LoopPlaybackSchedule schedule;
    };

    struct ActivePlayback {
        std::shared_ptr<const PlaybackTake> take;
        std::uint64_t generation{};
    };

    static bool isPlayablePeriod(Milliseconds period) noexcept;

    void workerMain(const std::stop_token& stop_token);
    bool waitForActivePlayback(
        const std::stop_token& stop_token,
        std::uint64_t& observed_generation,
        ActivePlayback& playback
    );
    void playActiveTake(
        const std::stop_token& stop_token,
        const ActivePlayback& playback
    );
    bool playCycle(
        const std::stop_token& stop_token,
        const ActivePlayback& playback,
        TimePoint loop_started_at,
        bool joining_first_cycle
    );
    void playRecordedEvent(const RecordedLoopEvent& event);

    SynthEngine& synth_engine_;
    SlotId id_;
    int channel_;

    mutable std::mutex playback_mutex_;
    std::condition_variable playback_changed_;
    std::shared_ptr<const PlaybackTake> committed_take_;
    std::uint64_t playback_generation_{};
    std::jthread worker_;
};

} // namespace zeta
