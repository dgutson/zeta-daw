#include "midi_take_player.hpp"

#include "synth_engine.hpp"

#include <iostream>
#include <stdexcept>
#include <syncstream>
#include <utility>

namespace zeta {

MidiTakePlayer::MidiTakePlayer(
    SynthEngine& synth_engine,
    // Slot identity and MIDI channel are distinct domain values.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    SlotId id,
    int channel
)
    : synth_engine_(synth_engine),
      id_(id),
      channel_(channel),
      worker_([this](const std::stop_token& stop_token) {
          workerMain(stop_token);
      }) {}

std::optional<LoopPlaybackSchedule> MidiTakePlayer::schedule() const {
    std::lock_guard lock(playback_mutex_);
    if (!committed_take_
        || !isPlayablePeriod(committed_take_->schedule.period)) {
        return std::nullopt;
    }
    return committed_take_->schedule;
}

void MidiTakePlayer::commitTake(
    const std::vector<RecordedLoopEvent>& events,
    const LoopPlaybackSchedule& schedule
) {
    auto take = std::make_shared<PlaybackTake>(PlaybackTake{
        .events = events,
        .schedule = schedule,
    });

    synth_engine_.allNotesOff(channel_);
    {
        std::lock_guard playback_lock(playback_mutex_);
        committed_take_ = std::move(take);
    }
}

bool MidiTakePlayer::isPlayablePeriod(Milliseconds period) noexcept {
    return period > Milliseconds::zero();
}

void MidiTakePlayer::activatePlayback() {
    {
        std::lock_guard lock(playback_mutex_);
        if (!committed_take_) {
            throw std::logic_error("Cannot start a loop slot without a take");
        }

        ++playback_generation_;
    }
    playback_changed_.notify_all();
}

void MidiTakePlayer::deactivatePlayback() {
    invalidateAndSilence();
}

void MidiTakePlayer::terminatePlayback() {
    worker_.request_stop();
    invalidateAndSilence();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void MidiTakePlayer::workerMain(const std::stop_token& stop_token) {
    std::uint64_t observed_generation = 0;

    while (!stop_token.stop_requested()) {
        ActivePlayback playback{};
        const bool activated = waitForActivePlayback(
            stop_token,
            observed_generation,
            playback
        );
        if (!activated) {
            continue;
        }

        playActiveTake(stop_token, playback);
    }
}

bool MidiTakePlayer::waitForActivePlayback(
    const std::stop_token& stop_token,
    std::uint64_t& observed_generation,
    ActivePlayback& playback
) {
    std::unique_lock lock(playback_mutex_);
    playback_changed_.wait(lock, [&] {
        return stop_token.stop_requested()
            || playback_generation_ != observed_generation;
    });

    if (stop_token.stop_requested()) {
        return false;
    }

    observed_generation = playback_generation_;
    if (!committed_take_) {
        return false;
    }

    playback = ActivePlayback{
        .take = committed_take_,
        .generation = observed_generation,
    };
    return true;
}

void MidiTakePlayer::playActiveTake(
    const std::stop_token& stop_token,
    const ActivePlayback& playback
) {
    const auto& take = *playback.take;
    if (!isPlayablePeriod(take.schedule.period)) {
        std::osyncstream{std::cerr}
            << "[loop slot playback error] zero-length take ignored\n";
        return;
    }

    auto loop_started_at = take.schedule.first_cycle_at;
    bool joining_first_cycle = true;

    while (!stop_token.stop_requested()) {
        const bool cycle_completed = playCycle(
            stop_token,
            playback,
            loop_started_at,
            joining_first_cycle
        );
        if (!cycle_completed) {
            return;
        }

        const auto loop_end = loop_started_at + take.schedule.period;
        std::unique_lock lock(playback_mutex_);
        playback_changed_.wait_until(lock, loop_end, [&] {
            return stop_token.stop_requested()
                || playback_generation_ != playback.generation;
        });

        if (stop_token.stop_requested()
            || playback_generation_ != playback.generation) {
            return;
        }

        loop_started_at += take.schedule.period;
        joining_first_cycle = false;
    }
}

bool MidiTakePlayer::playCycle(
    const std::stop_token& stop_token,
    const ActivePlayback& playback,
    TimePoint loop_started_at,
    bool joining_first_cycle
) {
    for (const auto& event : playback.take->events) {
        const auto deadline = loop_started_at + Milliseconds(event.time_ms);
        if (joining_first_cycle
            && deadline < playback.take->schedule.first_cycle_join_at) {
            continue;
        }

        std::unique_lock lock(playback_mutex_);
        playback_changed_.wait_until(lock, deadline, [&] {
            return stop_token.stop_requested()
                || playback_generation_ != playback.generation;
        });

        if (stop_token.stop_requested()
            || playback_generation_ != playback.generation) {
            return false;
        }

        playRecordedEvent(event);
    }
    return true;
}

void MidiTakePlayer::playRecordedEvent(const RecordedLoopEvent& event) {
    #ifdef ZETA_MIDI_TRACE
    std::osyncstream{std::cerr}
        << "[loop slot playback]"
        << " slot=" << id_
        << " channel=" << channel_
        << " kind=" << (
            event.kind == RecordedNoteKind::NoteOn ? "note_on" : "note_off"
        )
        << " key=" << event.key
        << " velocity=" << event.velocity
        << "\n";
    #endif

    if (event.kind == RecordedNoteKind::NoteOn) {
        synth_engine_.noteOn(channel_, event.key, event.velocity);
    } else {
        synth_engine_.noteOff(channel_, event.key);
    }
}

void MidiTakePlayer::invalidateAndSilence() {
    {
        std::lock_guard lock(playback_mutex_);
        committed_take_.reset();
        ++playback_generation_;
        synth_engine_.allNotesOff(channel_);
    }
    playback_changed_.notify_all();
}

} // namespace zeta
