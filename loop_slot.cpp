#include "loop_slot.hpp"

#include "synth_engine.hpp"

#include <iostream>
#include <stdexcept>
#include <syncstream>
#include <utility>

namespace zeta {
namespace {

constexpr int first_loop_slot_channel = 1;

} // namespace

LoopSlot::LoopSlot(
    SlotId id,
    const LoopSlotDefinition& definition,
    SynthEngine& synth_engine
)
    : id_(id),
      selection_key_(definition.key),
      channel_(first_loop_slot_channel + static_cast<int>(id)),
      synth_engine_(synth_engine),
      playback_fsm_(*this),
      worker_([this](std::stop_token stop_token) {
          workerMain(std::move(stop_token));
      }) {}

LoopSlot::~LoopSlot() {
    terminationRequested();
}

SlotId LoopSlot::id() const noexcept {
    return id_;
}

int LoopSlot::selectionKey() const noexcept {
    return selection_key_;
}

int LoopSlot::channel() const noexcept {
    return channel_;
}

LoopSlotPlaybackState LoopSlot::playbackState() const {
    std::lock_guard lock(command_mutex_);
    return playback_fsm_.state();
}

std::optional<LoopPlaybackSchedule> LoopSlot::activeSchedule() const {
    if (playbackState() != LoopSlotPlaybackState::Looping) {
        return std::nullopt;
    }

    std::lock_guard lock(playback_mutex_);
    if (!committed_take_
        || !isPlayablePeriod(committed_take_->schedule.period)) {
        return std::nullopt;
    }
    return committed_take_->schedule;
}

LoopSlotSelectionOutcome LoopSlot::selectionRequested(
    const LoopSlotSelectionContext& context
) {
    const auto state = playbackState();
    if (state == LoopSlotPlaybackState::Terminated) {
        return LoopSlotSelectionOutcome::Unavailable;
    }
    if (state == LoopSlotPlaybackState::Looping) {
        onLoopingSelection();
        return LoopSlotSelectionOutcome::Stopped;
    }
    return onMutedSelection(context);
}

LoopSlotSelectionOutcome LoopSlot::arm(
    const LoopSlotSelectionContext& context,
    std::optional<LoopPlaybackSchedule> guide
) {
    std::lock_guard command_lock(command_mutex_);
    if (playback_fsm_.state() != LoopSlotPlaybackState::Muted) {
        throw std::logic_error("Only a muted loop slot can be armed");
    }

    invalidateAndSilence();
    soundfont_ = &context.soundfont;
    transposer_ = context.transposer;
    prepared_guide_ = guide;
    synth_engine_.select(context.soundfont, channel_);
    return LoopSlotSelectionOutcome::Armed;
}

void LoopSlot::cancelRecording() {
    std::lock_guard lock(command_mutex_);
    if (playback_fsm_.state() != LoopSlotPlaybackState::Muted) {
        throw std::logic_error("Only a muted loop slot can cancel recording");
    }
    prepared_guide_.reset();
    invalidateAndSilence();
}

void LoopSlot::recordingCompleted(
    const std::vector<RecordedLoopEvent>& events,
    Milliseconds content_duration,
    const TakeTiming& timing
) {
    std::lock_guard command_lock(command_mutex_);
    if (playback_fsm_.state() != LoopSlotPlaybackState::Muted) {
        throw std::logic_error("Only a muted loop slot can complete recording");
    }
    if (!soundfont_) {
        throw std::logic_error("Cannot complete an unconfigured loop slot");
    }

    auto take = std::make_shared<PlaybackTake>(PlaybackTake{
        .events = events,
        .schedule = makeSchedule(timing, content_duration, prepared_guide_),
    });

    synth_engine_.allNotesOff(channel_);
    {
        std::lock_guard playback_lock(playback_mutex_);
        committed_take_ = std::move(take);
    }
    prepared_guide_.reset();
    playback_fsm_.startRequested();
}

void LoopSlot::selectSoundFont(const SoundFontDefinition& soundfont) {
    std::lock_guard lock(command_mutex_);
    soundfont_ = &soundfont;
    synth_engine_.select(soundfont, channel_);
}

void LoopSlot::octaveDown() {
    std::lock_guard lock(command_mutex_);
    transposer_.octaveDown();
}

void LoopSlot::octaveUp() {
    std::lock_guard lock(command_mutex_);
    transposer_.octaveUp();
}

MidiMessage LoopSlot::transpose(const MidiMessage& message) const {
    std::lock_guard lock(command_mutex_);
    return transposer_.transpose(message);
}

int LoopSlot::monitorMidi(const MidiMessage& message) {
    const auto transposed = transpose(message);
    return synth_engine_.send(transposed, channel_);
}

void LoopSlot::deactivate() {
    std::lock_guard lock(command_mutex_);
    playback_fsm_.muteRequested();
}

void LoopSlot::terminationRequested() {
    std::lock_guard lock(command_mutex_);
    playback_fsm_.terminationRequested();
}

bool LoopSlot::isPlayablePeriod(Milliseconds period) noexcept {
    return period > Milliseconds::zero();
}

void LoopSlot::activatePlayback() {
    {
        std::lock_guard lock(playback_mutex_);
        if (!committed_take_) {
            throw std::logic_error("Cannot start a loop slot without a take");
        }
        if (!soundfont_) {
            throw std::logic_error("Cannot start an unconfigured loop slot");
        }

        synth_engine_.select(*soundfont_, channel_);
        ++playback_generation_;
    }
    playback_changed_.notify_all();
}

void LoopSlot::deactivatePlayback() {
    invalidateAndSilence();
}

void LoopSlot::terminatePlayback() {
    worker_.request_stop();
    invalidateAndSilence();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void LoopSlot::workerMain(std::stop_token stop_token) {
    std::uint64_t observed_generation = 0;

    while (!stop_token.stop_requested()) {
        std::shared_ptr<const PlaybackTake> take;
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
            take = committed_take_;
            if (!take) {
                continue;
            }
            active_generation = observed_generation;
        }

        if (!isPlayablePeriod(take->schedule.period)) {
            std::osyncstream{std::cerr}
                << "[loop slot playback error] zero-length take ignored\n";
            continue;
        }

        auto loop_started_at = take->schedule.first_cycle_at;
        bool joining_first_cycle = true;
        bool interrupted = false;

        while (!stop_token.stop_requested() && !interrupted) {
            for (const auto& event : take->events) {
                const auto deadline =
                    loop_started_at + Milliseconds(event.time_ms);
                if (joining_first_cycle
                    && deadline < take->schedule.first_cycle_join_at) {
                    continue;
                }

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

            const auto loop_end = loop_started_at + take->schedule.period;
            std::unique_lock lock(playback_mutex_);
            playback_changed_.wait_until(lock, loop_end, [&] {
                return stop_token.stop_requested()
                    || playback_generation_ != active_generation;
            });

            if (stop_token.stop_requested()
                || playback_generation_ != active_generation) {
                break;
            }

            loop_started_at += take->schedule.period;
            joining_first_cycle = false;
        }
    }
}

void LoopSlot::playRecordedEvent(const RecordedLoopEvent& event) {
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

void LoopSlot::invalidateAndSilence() {
    {
        std::lock_guard lock(playback_mutex_);
        committed_take_.reset();
        ++playback_generation_;
        synth_engine_.allNotesOff(channel_);
    }
    playback_changed_.notify_all();
}

GuideLoopSlot::GuideLoopSlot(
    SlotId id,
    const LoopSlotDefinition& definition,
    SynthEngine& synth_engine,
    LoopSlotGroupOutput& output
) : LoopSlot(id, definition, synth_engine), output_(output) {}

LoopSlotSelectionOutcome GuideLoopSlot::onMutedSelection(
    const LoopSlotSelectionContext& context
) {
    return arm(context, std::nullopt);
}

void GuideLoopSlot::onLoopingSelection() {
    output_.stopDependentSlots();
    deactivate();
}

LoopPlaybackSchedule GuideLoopSlot::makeSchedule(
    const TakeTiming& timing,
    Milliseconds,
    const std::optional<LoopPlaybackSchedule>&
) const {
    return LoopPlaybackSchedule::forGuide(timing);
}

RegularLoopSlot::RegularLoopSlot(
    SlotId id,
    const LoopSlotDefinition& definition,
    SynthEngine& synth_engine
) : LoopSlot(id, definition, synth_engine) {}

LoopSlotSelectionOutcome RegularLoopSlot::onMutedSelection(
    const LoopSlotSelectionContext& context
) {
    if (!context.guide_schedule) {
        return LoopSlotSelectionOutcome::GuideRequired;
    }
    return arm(context, context.guide_schedule);
}

void RegularLoopSlot::onLoopingSelection() {
    deactivate();
}

LoopPlaybackSchedule RegularLoopSlot::makeSchedule(
    const TakeTiming& timing,
    Milliseconds content_duration,
    const std::optional<LoopPlaybackSchedule>& guide
) const {
    if (!guide) {
        throw std::logic_error("Regular slot recording has no guide schedule");
    }
    return LoopPlaybackSchedule::forRegular(
        timing,
        content_duration,
        guide.value()
    );
}

} // namespace zeta
