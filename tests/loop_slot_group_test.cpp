#include "fake_fluidsynth.hpp"
#include "integration_support.hpp"
#include "../loop_slot_group.hpp"
#include "../synth_engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace integration_support;
using fake_fluidsynth::CallKind;
using zeta::LoopSlotGroup;
using zeta::LoopSlotSelectionOutcome;
using zeta::LooperClock;
using zeta::Milliseconds;
using zeta::MidiMessage;
using zeta::MidiMessageType;
using zeta::OctaveTransposer;
using zeta::RecordedNoteKind;
using zeta::SoundFontDefinition;
using zeta::SynthEngine;
using zeta::TakeTiming;
using zeta::TimePoint;

void completeDirectGuide(
    LoopSlotGroup& slots,
    const SoundFontDefinition& soundfont,
    const OctaveTransposer& transposer,
    TimePoint first_cycle_at,
    Milliseconds period
) {
    const auto guide = slots.requestSelection(
        first_slot_key,
        soundfont,
        transposer
    );
    ASSERT_EQ(guide.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(
        guide.id,
        RecordedNoteKind::NoteOn,
        recordedNote(MidiMessageType::NoteOn, 72, 100),
        0ms
    );
    slots.recordNote(
        guide.id,
        RecordedNoteKind::NoteOff,
        recordedNote(MidiMessageType::NoteOff, 72),
        0ms
    );
    slots.completeRecording(guide.id, TakeTiming{
        .recording_started_at = first_cycle_at - period,
        .completed_at = first_cycle_at,
    });
}

class LoopSlotGroupTest : public ::testing::Test {
protected:
    void SetUp() override {
        fake_fluidsynth::reset();
    }
};

TEST_F(LoopSlotGroupTest, LateDependentDispatchKeepsNextGuideDeadline) {
    auto config = testConfig();
    SynthEngine synth_engine{config};
    LoopSlotGroup slots{config.loop_slots, synth_engine};
    OctaveTransposer transposer;
    const auto& soundfont = config.soundfonts.front();
    const MidiMessage note_on{
        .raw_type = raw(MidiMessageType::NoteOn),
        .key = 64,
        .velocity = 100,
    };
    const MidiMessage note_off{
        .raw_type = raw(MidiMessageType::NoteOff),
        .key = 64,
    };

    constexpr auto guide_period = 2000ms;
    constexpr auto dispatch_lateness = 1500ms;
    static_assert(dispatch_lateness < guide_period);
    const auto guide_first_cycle_at = LooperClock::now() - dispatch_lateness;

    const auto guide = slots.requestSelection(
        first_slot_key,
        soundfont,
        transposer
    );
    ASSERT_EQ(guide.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(guide.id, RecordedNoteKind::NoteOn, note_on, 0ms);
    slots.recordNote(guide.id, RecordedNoteKind::NoteOff, note_off, 0ms);
    slots.completeRecording(guide.id, TakeTiming{
        .recording_started_at = guide_first_cycle_at - guide_period,
        .completed_at = guide_first_cycle_at,
    });

    const auto dependent = slots.requestSelection(
        second_slot_key,
        soundfont,
        transposer
    );
    ASSERT_EQ(dependent.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(dependent.id, RecordedNoteKind::NoteOn, note_on, 0ms);
    slots.recordNote(dependent.id, RecordedNoteKind::NoteOff, note_off, 0ms);
    slots.completeRecording(dependent.id, TakeTiming{
        .recording_started_at = guide_first_cycle_at - guide_period,
        .completed_at = guide_first_cycle_at,
    });

    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 1));
    EXPECT_TRUE(waitForNoteCount(second_slot_channel, 64, 2, 1200ms));
}

TEST_F(
    LoopSlotGroupTest,
    RegularFirstCycleSkipsExpiredPrefixAndIncludesCompletionBoundary
) {
    auto config = testConfig();
    SynthEngine synth_engine{config};
    LoopSlotGroup slots{config.loop_slots, synth_engine};
    OctaveTransposer transposer;
    const auto& soundfont = config.soundfonts.front();

    constexpr auto guide_period = 400ms;
    constexpr auto elapsed_repetition = 80ms;
    const auto completed_at = LooperClock::now();
    const auto first_cycle_at = completed_at - elapsed_repetition;
    const auto recording_started_at = first_cycle_at - guide_period;
    completeDirectGuide(
        slots,
        soundfont,
        transposer,
        completed_at - guide_period / 2,
        guide_period
    );

    const auto regular = slots.requestSelection(
        second_slot_key,
        soundfont,
        transposer
    );
    ASSERT_EQ(regular.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOn,
        recordedNote(MidiMessageType::NoteOn, 64, 100),
        0ms
    );
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOn,
        recordedNote(MidiMessageType::NoteOn, 67, 100),
        elapsed_repetition
    );
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOff,
        recordedNote(MidiMessageType::NoteOff, 64),
        140ms
    );
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOff,
        recordedNote(MidiMessageType::NoteOff, 67),
        180ms
    );
    const auto silence_count = controlChangeCount(second_slot_channel, 123);

    slots.completeRecording(regular.id, TakeTiming{
        .recording_started_at = recording_started_at,
        .completed_at = completed_at,
    });

    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 67, 1, 200ms));
    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, second_slot_channel, 64),
        0U
    );
    ASSERT_TRUE(waitForNoteOffCount(second_slot_channel, 64, 1, 200ms));
    EXPECT_GT(controlChangeCount(second_slot_channel, 123), silence_count);

    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 1, 500ms));
    EXPECT_TRUE(waitForNoteCount(second_slot_channel, 67, 2, 200ms));
}

TEST_F(LoopSlotGroupTest, RegularWaitsWhenCurrentRepetitionEventsArePast) {
    auto config = testConfig();
    SynthEngine synth_engine{config};
    LoopSlotGroup slots{config.loop_slots, synth_engine};
    OctaveTransposer transposer;
    const auto& soundfont = config.soundfonts.front();

    constexpr auto guide_period = 400ms;
    constexpr auto elapsed_repetition = 180ms;
    const auto completed_at = LooperClock::now();
    const auto first_cycle_at = completed_at - elapsed_repetition;
    const auto recording_started_at = first_cycle_at - guide_period;
    completeDirectGuide(
        slots,
        soundfont,
        transposer,
        completed_at - guide_period / 2,
        guide_period
    );

    const auto regular = slots.requestSelection(
        second_slot_key,
        soundfont,
        transposer
    );
    ASSERT_EQ(regular.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOn,
        recordedNote(MidiMessageType::NoteOn, 64, 100),
        0ms
    );
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOff,
        recordedNote(MidiMessageType::NoteOff, 64),
        100ms
    );

    slots.completeRecording(regular.id, TakeTiming{
        .recording_started_at = recording_started_at,
        .completed_at = completed_at,
    });

    std::this_thread::sleep_for(75ms);
    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, second_slot_channel, 64),
        0U
    );
    EXPECT_TRUE(waitForNoteCount(second_slot_channel, 64, 1, 400ms));
    EXPECT_TRUE(waitForNoteOffCount(second_slot_channel, 64, 1, 200ms));
}

} // namespace
