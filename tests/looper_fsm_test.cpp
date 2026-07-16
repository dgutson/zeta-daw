#include "../looper_fsm.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace {

using namespace std::chrono_literals;
using ::testing::_;
using ::testing::InSequence;
using ::testing::Return;
using ::testing::StrictMock;
using zeta::LooperFsm;
using zeta::LooperOutput;
using zeta::LooperStateRegistry;
using zeta::LoopSlotView;
using zeta::Milliseconds;
using zeta::MidiMessage;
using zeta::MidiMessageType;
using zeta::MidiRoute;
using zeta::RecordedNoteKind;
using zeta::SlotId;
using zeta::SlotPlaybackState;
using zeta::StateId;
using zeta::TimePoint;

class MockOutput : public LooperOutput {
public:
    MOCK_METHOD(int, monitorMidi, (const MidiMessage&, MidiRoute), (override));
    MOCK_METHOD(void, selectCurrentSoundFont, (MidiRoute), (override));
    MOCK_METHOD(void, selectNextSoundFont, (MidiRoute), (override));
    MOCK_METHOD(void, selectSoundFontByNote, (MidiRoute, int), (override));
    MOCK_METHOD(void, octaveDown, (MidiRoute), (override));
    MOCK_METHOD(void, octaveUp, (MidiRoute), (override));
    MOCK_METHOD(void, prepareTake, (SlotId), (override));
    MOCK_METHOD(void, discardPendingTake, (SlotId), (override));
    MOCK_METHOD(
        void,
        recordNote,
        (SlotId, RecordedNoteKind, const MidiMessage&, Milliseconds),
        (override)
    );
    MOCK_METHOD(void, commitTake, (SlotId, Milliseconds), (override));
    MOCK_METHOD(void, startSlotPlayback, (SlotId), (override));
    MOCK_METHOD(void, muteSlotPlayback, (SlotId), (override));
    MOCK_METHOD(void, terminateSlots, (), (override));
    MOCK_METHOD(void, silenceAllChannels, (), (override));
    MOCK_METHOD(void, showRecordingArmed, (SlotId), (override));
    MOCK_METHOD(void, showLooping, (SlotId), (override));
    MOCK_METHOD(void, showMuted, (SlotId), (override));
    MOCK_METHOD(void, showNoTake, (SlotId), (override));
    MOCK_METHOD(void, showRecorderBusy, (SlotId), (override));
    MOCK_METHOD(void, showUnknownLoopSlot, (int), (override));
};

class MockSlotView : public LoopSlotView {
public:
    MOCK_METHOD(std::optional<SlotId>, slotByKey, (int), (const, override));
    MOCK_METHOD(bool, slotHasTake, (SlotId), (const, override));
    MOCK_METHOD(
        SlotPlaybackState,
        slotPlaybackState,
        (SlotId),
        (const, override)
    );
};

constexpr SlotId first_slot = 0;
constexpr SlotId second_slot = 1;
constexpr int first_slot_key = 60;
constexpr int second_slot_key = 62;
constexpr auto start_time = TimePoint{} + 100ms;

MidiMessage positiveNote(int key = 67) {
    return {.channel = 0, .key = key, .velocity = 100};
}

void selectEmptySlot(
    LooperFsm& fsm,
    StrictMock<MockOutput>& output,
    StrictMock<MockSlotView>& slots,
    SlotId slot = first_slot,
    int key = first_slot_key
) {
    EXPECT_EQ(fsm.loopSlotControlPressed(), StateId::ReadySelectingLoopSlot);
    EXPECT_CALL(slots, slotByKey(key)).WillOnce(Return(slot));
    EXPECT_CALL(slots, slotHasTake(slot)).WillOnce(Return(false));
    {
        InSequence sequence;
        EXPECT_CALL(output, prepareTake(slot));
        EXPECT_CALL(output, showRecordingArmed(slot));
    }
    auto selection = positiveNote(key);
    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, selection, start_time),
        0
    );
    EXPECT_EQ(fsm.stateId(), StateId::Armed);
}

void beginRecording(
    LooperFsm& fsm,
    StrictMock<MockOutput>& output
) {
    auto note = positiveNote();
    EXPECT_CALL(
        output,
        monitorMidi(_, MidiRoute::recordingSlot(first_slot))
    ).WillOnce(Return(17));
    EXPECT_CALL(
        output,
        recordNote(first_slot, RecordedNoteKind::NoteOn, _, 0ms)
    );
    EXPECT_EQ(fsm.midiMessage(MidiMessageType::NoteOn, note, start_time), 17);
    EXPECT_EQ(fsm.stateId(), StateId::Recording);
}

TEST(LooperFsmTest, ReadyRoutesLiveMidiAndLiveControls) {
    StrictMock<MockOutput> output;
    StrictMock<MockSlotView> slots;
    LooperStateRegistry states{output, slots};
    LooperFsm fsm{states};
    auto note = positiveNote();

    EXPECT_CALL(output, monitorMidi(_, MidiRoute::live())).WillOnce(Return(23));
    EXPECT_EQ(fsm.midiMessage(MidiMessageType::NoteOn, note, start_time), 23);

    EXPECT_CALL(output, selectNextSoundFont(MidiRoute::live()));
    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Ready);
    EXPECT_CALL(output, octaveDown(MidiRoute::live()));
    EXPECT_EQ(fsm.octaveDownPressed(), StateId::Ready);
    EXPECT_CALL(output, octaveUp(MidiRoute::live()));
    EXPECT_EQ(fsm.octaveUpPressed(), StateId::Ready);
}

TEST(LooperFsmTest, EmptySlotArmsAndFirstPositiveNoteStartsAtOffsetZero) {
    StrictMock<MockOutput> output;
    StrictMock<MockSlotView> slots;
    LooperStateRegistry states{output, slots};
    LooperFsm fsm{states};

    selectEmptySlot(fsm, output, slots);
    beginRecording(fsm, output);
}

TEST(LooperFsmTest, RecordingCompletesOnlyAfterSelectingItsSlot) {
    StrictMock<MockOutput> output;
    StrictMock<MockSlotView> slots;
    LooperStateRegistry states{output, slots};
    LooperFsm fsm{states};

    selectEmptySlot(fsm, output, slots);
    beginRecording(fsm, output);

    EXPECT_EQ(
        fsm.loopSlotControlPressed(),
        StateId::RecordingSelectingLoopSlot
    );
    EXPECT_CALL(slots, slotByKey(first_slot_key)).WillOnce(Return(first_slot));
    {
        InSequence sequence;
        EXPECT_CALL(output, commitTake(first_slot, 25ms));
        EXPECT_CALL(output, selectCurrentSoundFont(MidiRoute::live()));
        EXPECT_CALL(output, startSlotPlayback(first_slot));
        EXPECT_CALL(output, showLooping(first_slot));
    }
    auto selection = positiveNote(first_slot_key);
    fsm.midiMessage(
        MidiMessageType::NoteOn,
        selection,
        start_time + 25ms
    );
    EXPECT_EQ(fsm.stateId(), StateId::Ready);
}

TEST(LooperFsmTest, RecordedSlotSelectionStartsOrMutesOnlyThatSlot) {
    StrictMock<MockOutput> output;
    StrictMock<MockSlotView> slots;
    LooperStateRegistry states{output, slots};
    LooperFsm fsm{states};
    auto selection = positiveNote(first_slot_key);

    EXPECT_EQ(fsm.loopSlotControlPressed(), StateId::ReadySelectingLoopSlot);
    EXPECT_CALL(slots, slotByKey(first_slot_key)).WillOnce(Return(first_slot));
    EXPECT_CALL(slots, slotHasTake(first_slot)).WillOnce(Return(true));
    EXPECT_CALL(slots, slotPlaybackState(first_slot))
        .WillOnce(Return(SlotPlaybackState::Muted));
    {
        InSequence sequence;
        EXPECT_CALL(output, startSlotPlayback(first_slot));
        EXPECT_CALL(output, showLooping(first_slot));
    }
    fsm.midiMessage(MidiMessageType::NoteOn, selection, start_time);

    EXPECT_EQ(fsm.loopSlotControlPressed(), StateId::ReadySelectingLoopSlot);
    EXPECT_CALL(slots, slotByKey(first_slot_key)).WillOnce(Return(first_slot));
    EXPECT_CALL(slots, slotHasTake(first_slot)).WillOnce(Return(true));
    EXPECT_CALL(slots, slotPlaybackState(first_slot))
        .WillOnce(Return(SlotPlaybackState::Looping));
    {
        InSequence sequence;
        EXPECT_CALL(output, muteSlotPlayback(first_slot));
        EXPECT_CALL(output, showMuted(first_slot));
    }
    fsm.midiMessage(MidiMessageType::NoteOn, selection, start_time);
    EXPECT_EQ(fsm.stateId(), StateId::Ready);
}

TEST(LooperFsmTest, OtherPlayingSlotsCanBeMutedWhileRecordingContinues) {
    StrictMock<MockOutput> output;
    StrictMock<MockSlotView> slots;
    LooperStateRegistry states{output, slots};
    LooperFsm fsm{states};

    selectEmptySlot(fsm, output, slots);
    beginRecording(fsm, output);
    EXPECT_EQ(
        fsm.loopSlotControlPressed(),
        StateId::RecordingSelectingLoopSlot
    );

    EXPECT_CALL(slots, slotByKey(second_slot_key)).WillOnce(Return(second_slot));
    EXPECT_CALL(slots, slotHasTake(second_slot)).WillOnce(Return(true));
    EXPECT_CALL(slots, slotPlaybackState(second_slot))
        .WillOnce(Return(SlotPlaybackState::Looping));
    EXPECT_CALL(output, muteSlotPlayback(second_slot));
    EXPECT_CALL(output, showMuted(second_slot));
    auto selection = positiveNote(second_slot_key);
    fsm.midiMessage(MidiMessageType::NoteOn, selection, start_time + 10ms);
    EXPECT_EQ(fsm.stateId(), StateId::Recording);

    auto selection_release = MidiMessage{.key = second_slot_key};
    fsm.midiMessage(
        MidiMessageType::NoteOff,
        selection_release,
        start_time + 11ms
    );
    EXPECT_EQ(fsm.stateId(), StateId::Recording);
}

TEST(LooperFsmTest, ArmedSlotSelectionCancelsOnlyItsPendingTake) {
    StrictMock<MockOutput> output;
    StrictMock<MockSlotView> slots;
    LooperStateRegistry states{output, slots};
    LooperFsm fsm{states};

    selectEmptySlot(fsm, output, slots);
    EXPECT_EQ(fsm.loopSlotControlPressed(), StateId::ArmedSelectingLoopSlot);
    EXPECT_CALL(slots, slotByKey(first_slot_key)).WillOnce(Return(first_slot));
    {
        InSequence sequence;
        EXPECT_CALL(output, discardPendingTake(first_slot));
        EXPECT_CALL(output, selectCurrentSoundFont(MidiRoute::live()));
        EXPECT_CALL(output, showNoTake(first_slot));
    }
    auto selection = positiveNote(first_slot_key);
    fsm.midiMessage(MidiMessageType::NoteOn, selection, start_time);
    EXPECT_EQ(fsm.stateId(), StateId::Ready);
}

TEST(LooperFsmTest, EmptyPeerCannotAcquireTheBusyRecorder) {
    StrictMock<MockOutput> output;
    StrictMock<MockSlotView> slots;
    LooperStateRegistry states{output, slots};
    LooperFsm fsm{states};

    selectEmptySlot(fsm, output, slots);
    EXPECT_EQ(fsm.loopSlotControlPressed(), StateId::ArmedSelectingLoopSlot);
    EXPECT_CALL(slots, slotByKey(second_slot_key)).WillOnce(Return(second_slot));
    EXPECT_CALL(slots, slotHasTake(second_slot)).WillOnce(Return(false));
    EXPECT_CALL(output, showRecorderBusy(second_slot));
    auto selection = positiveNote(second_slot_key);
    fsm.midiMessage(MidiMessageType::NoteOn, selection, start_time);
    EXPECT_EQ(fsm.stateId(), StateId::Armed);
}

TEST(LooperFsmTest, ArmedSoundFontAndOctaveControlsTargetSelectedSlot) {
    StrictMock<MockOutput> output;
    StrictMock<MockSlotView> slots;
    LooperStateRegistry states{output, slots};
    LooperFsm fsm{states};

    selectEmptySlot(fsm, output, slots);
    EXPECT_CALL(
        output,
        selectNextSoundFont(MidiRoute::recordingSlot(first_slot))
    );
    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Armed);

    EXPECT_CALL(output, octaveDown(MidiRoute::live()));
    EXPECT_CALL(
        output,
        octaveDown(MidiRoute::recordingSlot(first_slot))
    );
    EXPECT_EQ(fsm.octaveDownPressed(), StateId::Armed);

    EXPECT_EQ(
        fsm.soundFontByNotePressed(),
        StateId::ArmedSelectingSoundFont
    );
    EXPECT_CALL(
        output,
        selectSoundFontByNote(MidiRoute::recordingSlot(first_slot), 72)
    );
    auto selection = positiveNote(72);
    fsm.midiMessage(MidiMessageType::NoteOn, selection, start_time);
    EXPECT_EQ(fsm.stateId(), StateId::Armed);
}

TEST(LooperFsmTest, UnknownSlotNoteIsConsumedAndLeavesSelection) {
    StrictMock<MockOutput> output;
    StrictMock<MockSlotView> slots;
    LooperStateRegistry states{output, slots};
    LooperFsm fsm{states};
    auto selection = positiveNote(99);

    EXPECT_EQ(fsm.loopSlotControlPressed(), StateId::ReadySelectingLoopSlot);
    EXPECT_CALL(slots, slotByKey(99)).WillOnce(Return(std::nullopt));
    EXPECT_CALL(output, showUnknownLoopSlot(99));
    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, selection, start_time),
        0
    );
    EXPECT_EQ(fsm.stateId(), StateId::Ready);
}

TEST(LooperFsmTest, ShutdownTerminatesAllSlotsAndIsIdempotent) {
    StrictMock<MockOutput> output;
    StrictMock<MockSlotView> slots;
    LooperStateRegistry states{output, slots};
    LooperFsm fsm{states};

    {
        InSequence sequence;
        EXPECT_CALL(output, terminateSlots());
        EXPECT_CALL(output, silenceAllChannels());
    }
    EXPECT_EQ(fsm.shutdownRequested(), StateId::Stopped);
    EXPECT_FALSE(fsm.shouldRun());
    EXPECT_EQ(fsm.shutdownRequested(), StateId::Stopped);
    EXPECT_EQ(fsm.loopSlotControlPressed(), StateId::Stopped);
}

} // namespace
