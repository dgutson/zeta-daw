#include "loop_slot.hpp"

#include "configuration.hpp"
#include "synth_engine.hpp"

#include <iostream>
#include <stdexcept>
#include <syncstream>
#include <utility>

namespace zeta {

LoopSlot::LoopSlot(LoopSlotConfig config, SynthEngine& synth)
    : id_(config.id),
      selection_key_(config.selection_key),
      synth_channel_(config.synth_channel),
      synth_(synth),
      playback_fsm_(*this) {
    events_.reserve(max_recorded_events);
    worker_ = std::jthread([this](std::stop_token stop_token) {
        playbackMain(std::move(stop_token));
    });
}

LoopSlot::~LoopSlot() {
    terminationRequested();
}

SlotId LoopSlot::id() const noexcept {
    return id_;
}

int LoopSlot::selectionKey() const noexcept {
    return selection_key_;
}

int LoopSlot::synthChannel() const noexcept {
    return synth_channel_;
}

SlotPlaybackState LoopSlot::playbackState() const {
    return playback_fsm_.state();
}

bool LoopSlot::hasTake() const noexcept {
    return has_take_;
}

void LoopSlot::prepareTake(
    const SoundFontDefinition& soundfont,
    const OctaveTransposer& live_transposer
) {
    if (playbackState() != SlotPlaybackState::Muted || has_take_) {
        throw std::logic_error("Only an empty muted loop slot can be recorded");
    }
    transposer_ = live_transposer;
    selectSoundFont(soundfont);
    events_.clear();
    duration_ = SlotDuration::zero();
}

int LoopSlot::monitorMidi(const MidiMessage& message) {
    return synth_.send(transposer_.transpose(message), synth_channel_);
}

void LoopSlot::octaveDown() noexcept {
    transposer_.octaveDown();
}

void LoopSlot::octaveUp() noexcept {
    transposer_.octaveUp();
}

void LoopSlot::selectSoundFont(const SoundFontDefinition& soundfont) {
    synth_.select(soundfont, synth_channel_);
}

void LoopSlot::recordNote(
    RecordedNoteKind kind,
    const MidiMessage& message,
    SlotDuration offset
) {
    if (events_.size() >= max_recorded_events) {
        return;
    }

    const auto transposed = transposer_.transpose(message);
    events_.push_back({
        .time_ms = static_cast<std::uint64_t>(offset.count()),
        .key = transposed.key,
        .velocity = transposed.velocity,
        .kind = kind,
    });
}

void LoopSlot::commitTake(SlotDuration duration) noexcept {
    duration_ = duration;
    has_take_ = true;
}

void LoopSlot::discardPendingTake() noexcept {
    events_.clear();
    duration_ = SlotDuration::zero();
    has_take_ = false;
}

SlotPlaybackState LoopSlot::startRequested() {
    if (playbackState() == SlotPlaybackState::Terminated) {
        return SlotPlaybackState::Terminated;
    }
    if (!has_take_) {
        throw std::logic_error("Cannot start an empty loop slot");
    }
    return playback_fsm_.startRequested();
}

SlotPlaybackState LoopSlot::muteRequested() {
    return playback_fsm_.muteRequested();
}

SlotPlaybackState LoopSlot::terminationRequested() {
    return playback_fsm_.terminationRequested();
}

void LoopSlot::activatePlayback() {
    {
        std::lock_guard lock(playback_mutex_);
        playback_active_ = true;
        ++playback_generation_;
    }
    playback_changed_.notify_all();
}

void LoopSlot::deactivatePlayback() {
    {
        std::lock_guard lock(playback_mutex_);
        playback_active_ = false;
        ++playback_generation_;
    }
    playback_changed_.notify_all();
    synth_.silenceChannel(synth_channel_);
}

void LoopSlot::terminatePlayback() {
    if (worker_.joinable()) {
        worker_.request_stop();
        {
            std::lock_guard lock(playback_mutex_);
            playback_active_ = false;
            ++playback_generation_;
        }
        playback_changed_.notify_all();
        worker_.join();
    }
    synth_.silenceChannel(synth_channel_);
}

void LoopSlot::playbackMain(std::stop_token stop_token) {
    std::uint64_t observed_generation = 0;

    while (!stop_token.stop_requested()) {
        SlotDuration take_duration{};
        std::uint64_t active_generation = 0;

        {
            std::unique_lock lock(playback_mutex_);
            playback_changed_.wait(lock, [&] {
                return stop_token.stop_requested()
                    || playback_generation_ != observed_generation;
            });

            if (stop_token.stop_requested()) {
                break;
            }

            observed_generation = playback_generation_;
            if (!playback_active_) {
                continue;
            }

            take_duration = duration_;
            active_generation = observed_generation;
        }

        if (take_duration <= SlotDuration::zero()) {
            std::osyncstream{std::cerr}
                << "[loop playback error] zero-length take ignored\n";
            continue;
        }

        auto loop_started_at = std::chrono::steady_clock::now();
        bool interrupted = false;

        while (!stop_token.stop_requested() && !interrupted) {
            for (const auto& event : events_) {
                const auto deadline = loop_started_at
                    + SlotDuration(event.time_ms);
                std::unique_lock lock(playback_mutex_);
                playback_changed_.wait_until(lock, deadline, [&] {
                    return stop_token.stop_requested()
                        || playback_generation_ != active_generation;
                });

                if (stop_token.stop_requested()
                    || playback_generation_ != active_generation) {
                    interrupted = true;
                    break;
                }

                playRecordedEvent(event);
            }

            if (interrupted) {
                break;
            }

            const auto loop_end = loop_started_at + take_duration;
            std::unique_lock lock(playback_mutex_);
            playback_changed_.wait_until(lock, loop_end, [&] {
                return stop_token.stop_requested()
                    || playback_generation_ != active_generation;
            });

            if (stop_token.stop_requested()
                || playback_generation_ != active_generation) {
                break;
            }

            loop_started_at += take_duration;
        }
    }
}

void LoopSlot::playRecordedEvent(const RecordedNoteEvent& event) {
    #ifdef ZETA_MIDI_TRACE
    std::osyncstream{std::cerr}
        << "[loop playback]"
        << " slot=" << id_
        << " channel=" << synth_channel_
        << " kind=" << (
            event.kind == RecordedNoteKind::NoteOn ? "note_on" : "note_off"
        )
        << " key=" << event.key
        << " velocity=" << event.velocity
        << '\n';
    #endif

    if (event.kind == RecordedNoteKind::NoteOn) {
        synth_.noteOn(synth_channel_, event.key, event.velocity);
    } else {
        synth_.noteOff(synth_channel_, event.key);
    }
}

} // namespace zeta
