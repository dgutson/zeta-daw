#include "loop_slot.hpp"

#include "synth_engine.hpp"

#include <stdexcept>

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
      player_(synth_engine, id, channel_),
      playback_fsm_(*this) {}

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
    return player_.schedule();
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

    player_.invalidateAndSilence();
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
    player_.invalidateAndSilence();
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

    player_.commitTake(
        events,
        makeSchedule(timing, content_duration, prepared_guide_)
    );
    prepared_guide_.reset();
    synth_engine_.select(*soundfont_, channel_);
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

void LoopSlot::activatePlayback() {
    player_.activatePlayback();
}

void LoopSlot::deactivatePlayback() {
    player_.deactivatePlayback();
}

void LoopSlot::terminatePlayback() {
    player_.terminatePlayback();
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
