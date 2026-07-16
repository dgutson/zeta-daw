#include "../looper_fsm.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace {

using namespace std::chrono_literals;
using testing::_;
using testing::InSequence;
using testing::NiceMock;
using testing::Return;
using zeta::LoopSlotSelection;
using zeta::LooperClock;
using zeta::LooperFsm;
using zeta::LooperOutput;
using zeta::LooperStateRegistry;
using zeta::MidiMessage;
using zeta::MidiMessageType;
using zeta::Milliseconds;
using zeta::RecordedNoteKind;
using zeta::SelectableLoopSlotState;
using zeta::SlotId;
using zeta::StateId;
using zeta::TimePoint;

class MockLooperOutput : public LooperOutput {
public:
    MOCK_METHOD(int, monitorLiveMidi, (const MidiMessage&), (override));
    MOCK_METHOD(
        int,
        monitorLoopSlotMidi,
        (SlotId, const MidiMessage&),
        (override)
    );
    MOCK_METHOD(
        std::optional<LoopSlotSelection>,
        loopSlotByKey,
        (int),
        (const, override)
    );
    MOCK_METHOD(void, prepareLoopSlot, (SlotId), (override));
    MOCK_METHOD(void, cancelLoopSlotRecording, (SlotId), (override));
    MOCK_METHOD(void, muteLoopSlot, (SlotId), (override));
    MOCK_METHOD(void, startLoopSlot, (SlotId), (override));
    MOCK_METHOD(void, terminateLoopSlots, (), (override));
    MOCK_METHOD(void, selectCurrentLiveSoundFont, (), (override));
    MOCK_METHOD(void, selectNextLiveSoundFont, (), (override));
    MOCK_METHOD(void, selectNextLoopSlotSoundFont, (SlotId), (override));
    MOCK_METHOD(void, selectLiveSoundFontByNote, (int), (override));
    MOCK_METHOD(
        void,
        selectLoopSlotSoundFontByNote,
        (SlotId, int),
        (override)
    );
    MOCK_METHOD(void, octaveDownLive, (), (override));
    MOCK_METHOD(void, octaveUpLive, (), (override));
    MOCK_METHOD(void, octaveDownLoopSlot, (SlotId), (override));
    MOCK_METHOD(void, octaveUpLoopSlot, (SlotId), (override));
    MOCK_METHOD(void, resetPendingTake, (), (override));
    MOCK_METHOD(
        void,
        recordNote,
        (SlotId, RecordedNoteKind, const MidiMessage&, Milliseconds),
        (override)
    );
    MOCK_METHOD(void, commitTake, (SlotId, Milliseconds), (override));
    MOCK_METHOD(void, showRecordingArmed, (SlotId), (override));
    MOCK_METHOD(void, showLooping, (SlotId), (override));
    MOCK_METHOD(void, showNoTake, (SlotId), (override));
    MOCK_METHOD(void, silenceAllChannels, (), (override));
};

constexpr SlotId selected_slot = 2;
const TimePoint start_time = LooperClock::now();

MidiMessage noteOn(int key = 60, int velocity = 100, int channel = 0) {
    return {
        .raw_type = static_cast<int>(MidiMessageType::NoteOn),
        .channel = channel,
        .key = key,
        .velocity = velocity,
    };
}

MidiMessage noteOff(int key = 60) {
    return {
        .raw_type = static_cast<int>(MidiMessageType::NoteOff),
        .key = key,
    };
}

class Harness {
public:
    Harness() : registry(output), fsm(registry) {
        ON_CALL(output, monitorLiveMidi).WillByDefault(Return(17));
        ON_CALL(output, monitorLoopSlotMidi).WillByDefault(Return(23));
    }

    void selectMutedSlot(SlotId slot = selected_slot) {
        ON_CALL(output, loopSlotByKey).WillByDefault(Return(LoopSlotSelection{
            .id = slot,
            .state = SelectableLoopSlotState::Muted,
        }));
        fsm.loopSlotControlPressed(start_time);
        fsm.midiMessage(MidiMessageType::NoteOn, noteOn(48), start_time);
    }

    void startRecording(SlotId slot = selected_slot) {
        selectMutedSlot(slot);
        fsm.midiMessage(MidiMessageType::NoteOn, noteOn(), start_time);
    }

    NiceMock<MockLooperOutput> output;
    LooperStateRegistry registry;
    LooperFsm fsm;
};

TEST(LooperFsmTest, ReadyRoutesLiveAndEntersEachSelector) {
    Harness harness;
    auto message = noteOn();

    EXPECT_CALL(harness.output, monitorLiveMidi).WillOnce(Return(41));
    EXPECT_EQ(
        harness.fsm.midiMessage(MidiMessageType::NoteOn, message, start_time),
        41
    );
    EXPECT_EQ(harness.fsm.stateId(), StateId::Ready);

    EXPECT_EQ(
        harness.fsm.loopSlotControlPressed(start_time),
        StateId::ReadySelectingLoopSlot
    );
    EXPECT_EQ(
        harness.fsm.soundFontByNotePressed(),
        StateId::ReadySelectingSoundFont
    );
}

TEST(LooperFsmTest, LoopSlotSelectionConsumesRawNoteAndArmsMutedSlot) {
    Harness harness;
    harness.fsm.loopSlotControlPressed(start_time);

    InSequence sequence;
    EXPECT_CALL(harness.output, loopSlotByKey(48)).WillOnce(Return(
        LoopSlotSelection{
            .id = selected_slot,
            .state = SelectableLoopSlotState::Muted,
        }
    ));
    EXPECT_CALL(harness.output, prepareLoopSlot(selected_slot));
    EXPECT_CALL(harness.output, resetPendingTake());
    EXPECT_CALL(harness.output, showRecordingArmed(selected_slot));

    EXPECT_EQ(
        harness.fsm.midiMessage(
            MidiMessageType::NoteOn,
            noteOn(48, 100, 15),
            start_time
        ),
        0
    );
    EXPECT_EQ(harness.fsm.stateId(), StateId::Armed);
}

TEST(LooperFsmTest, LoopSlotSelectionStopsOnlyLoopingSlot) {
    Harness harness;
    harness.fsm.loopSlotControlPressed(start_time);

    EXPECT_CALL(harness.output, loopSlotByKey(50)).WillOnce(Return(
        LoopSlotSelection{
            .id = selected_slot,
            .state = SelectableLoopSlotState::Looping,
        }
    ));
    EXPECT_CALL(harness.output, muteLoopSlot(selected_slot));

    EXPECT_EQ(
        harness.fsm.midiMessage(
            MidiMessageType::NoteOn,
            noteOn(50),
            start_time
        ),
        0
    );
    EXPECT_EQ(harness.fsm.stateId(), StateId::Ready);
}

TEST(LooperFsmTest, LoopSlotSelectorCancelsOrConsumesUnmappedNote) {
    Harness harness;
    EXPECT_EQ(
        harness.fsm.loopSlotControlPressed(start_time),
        StateId::ReadySelectingLoopSlot
    );
    EXPECT_EQ(
        harness.fsm.loopSlotControlPressed(start_time),
        StateId::Ready
    );

    harness.fsm.loopSlotControlPressed(start_time);
    EXPECT_CALL(harness.output, loopSlotByKey(99)).WillOnce(Return(std::nullopt));
    EXPECT_EQ(
        harness.fsm.midiMessage(
            MidiMessageType::NoteOn,
            noteOn(99),
            start_time
        ),
        0
    );
    EXPECT_EQ(harness.fsm.stateId(), StateId::Ready);
}

TEST(LooperFsmTest, LoopSlotSelectorRetainsStateForOtherMidi) {
    Harness harness;
    harness.fsm.loopSlotControlPressed(start_time);
    auto message = noteOff();

    EXPECT_CALL(harness.output, monitorLiveMidi).WillOnce(Return(31));
    EXPECT_EQ(
        harness.fsm.midiMessage(MidiMessageType::NoteOff, message, start_time),
        31
    );
    EXPECT_EQ(harness.fsm.stateId(), StateId::ReadySelectingLoopSlot);
}

TEST(LooperFsmTest, SoundFontSelectorsConsumeNotesOnStateSpecificRoutes) {
    Harness ready;
    ready.fsm.soundFontByNotePressed();
    EXPECT_CALL(ready.output, monitorLiveMidi).WillOnce(Return(19));
    EXPECT_EQ(
        ready.fsm.midiMessage(
            MidiMessageType::NoteOff,
            noteOff(72),
            start_time
        ),
        19
    );
    EXPECT_EQ(ready.fsm.stateId(), StateId::ReadySelectingSoundFont);
    EXPECT_CALL(ready.output, selectLiveSoundFontByNote(72));
    EXPECT_EQ(
        ready.fsm.midiMessage(
            MidiMessageType::NoteOn,
            noteOn(72),
            start_time
        ),
        0
    );
    EXPECT_EQ(ready.fsm.stateId(), StateId::Ready);

    Harness armed;
    armed.selectMutedSlot();
    armed.fsm.soundFontByNotePressed();
    EXPECT_CALL(
        armed.output,
        monitorLoopSlotMidi(selected_slot, _)
    ).WillOnce(Return(29));
    EXPECT_EQ(
        armed.fsm.midiMessage(
            MidiMessageType::NoteOff,
            noteOff(71),
            start_time
        ),
        29
    );
    EXPECT_EQ(armed.fsm.stateId(), StateId::ArmedSelectingSoundFont);
    EXPECT_CALL(
        armed.output,
        selectLoopSlotSoundFontByNote(selected_slot, 71)
    );
    EXPECT_EQ(
        armed.fsm.midiMessage(
            MidiMessageType::NoteOn,
            noteOn(71),
            start_time
        ),
        0
    );
    EXPECT_EQ(armed.fsm.stateId(), StateId::Armed);
}

TEST(LooperFsmTest, SelectorControlsSwitchOrCancelExplicitStates) {
    Harness harness;
    harness.fsm.loopSlotControlPressed(start_time);
    EXPECT_EQ(
        harness.fsm.soundFontByNotePressed(),
        StateId::ReadySelectingSoundFont
    );
    EXPECT_EQ(
        harness.fsm.loopSlotControlPressed(start_time),
        StateId::ReadySelectingLoopSlot
    );
    harness.fsm.soundFontByNotePressed();
    EXPECT_EQ(harness.fsm.soundFontByNotePressed(), StateId::Ready);

    harness.selectMutedSlot();
    harness.fsm.soundFontByNotePressed();
    EXPECT_EQ(harness.fsm.soundFontByNotePressed(), StateId::Armed);
}

TEST(LooperFsmTest, FirstPositiveNoteStartsAtOffsetZeroOnSelectedSlot) {
    Harness harness;
    harness.selectMutedSlot();

    auto release = noteOff();
    EXPECT_CALL(
        harness.output,
        monitorLoopSlotMidi(selected_slot, _)
    ).WillOnce(Return(25));
    EXPECT_EQ(
        harness.fsm.midiMessage(
            MidiMessageType::NoteOff,
            release,
            start_time - 5ms
        ),
        25
    );
    EXPECT_EQ(harness.fsm.stateId(), StateId::Armed);

    EXPECT_CALL(
        harness.output,
        monitorLoopSlotMidi(selected_slot, _)
    ).WillOnce(Return(27));
    EXPECT_CALL(
        harness.output,
        recordNote(selected_slot, RecordedNoteKind::NoteOn, _, 0ms)
    );
    EXPECT_EQ(
        harness.fsm.midiMessage(
            MidiMessageType::NoteOn,
            noteOn(),
            start_time
        ),
        27
    );
    EXPECT_EQ(harness.fsm.stateId(), StateId::Recording);
}

TEST(LooperFsmTest, ArmedControlCancelsPendingTakeWithoutSlotNote) {
    for (const bool soundfont_selector_active : {false, true}) {
        Harness harness;
        harness.selectMutedSlot();
        if (soundfont_selector_active) {
            harness.fsm.soundFontByNotePressed();
        }

        InSequence sequence;
        EXPECT_CALL(harness.output, cancelLoopSlotRecording(selected_slot));
        EXPECT_CALL(harness.output, resetPendingTake());
        EXPECT_CALL(harness.output, selectCurrentLiveSoundFont());
        EXPECT_CALL(harness.output, showNoTake(selected_slot));
        EXPECT_EQ(
            harness.fsm.loopSlotControlPressed(start_time),
            StateId::Ready
        );
    }
}

TEST(LooperFsmTest, LoopSlotControlCompletesAndStartsSelectedSlot) {
    Harness harness;
    harness.startRecording();

    InSequence sequence;
    EXPECT_CALL(harness.output, commitTake(selected_slot, 37ms));
    EXPECT_CALL(harness.output, startLoopSlot(selected_slot));
    EXPECT_CALL(harness.output, selectCurrentLiveSoundFont());
    EXPECT_CALL(harness.output, showLooping(selected_slot));

    EXPECT_EQ(
        harness.fsm.loopSlotControlPressed(start_time + 37ms),
        StateId::Ready
    );
}

TEST(LooperFsmTest, RecordingStoresNotesAtClampedElapsedOffsets) {
    Harness harness;
    harness.startRecording();

    EXPECT_CALL(harness.output, monitorLoopSlotMidi(selected_slot, _));
    EXPECT_CALL(
        harness.output,
        recordNote(selected_slot, RecordedNoteKind::NoteOff, _, 12ms)
    );
    harness.fsm.midiMessage(
        MidiMessageType::NoteOff,
        noteOff(),
        start_time + 12ms
    );

    EXPECT_CALL(harness.output, monitorLoopSlotMidi(selected_slot, _));
    EXPECT_CALL(
        harness.output,
        recordNote(selected_slot, RecordedNoteKind::NoteOff, _, 0ms)
    );
    harness.fsm.midiMessage(
        MidiMessageType::NoteOn,
        noteOn(60, 0),
        start_time - 1ms
    );
    EXPECT_EQ(harness.fsm.stateId(), StateId::Recording);
}

TEST(LooperFsmTest, SoundFontAndOctaveControlsUseStateSpecificOwners) {
    Harness ready;
    EXPECT_CALL(ready.output, selectNextLiveSoundFont());
    EXPECT_EQ(ready.fsm.nextSoundFontPressed(), StateId::Ready);
    EXPECT_CALL(ready.output, octaveDownLive());
    EXPECT_EQ(ready.fsm.octaveDownPressed(), StateId::Ready);

    Harness selecting;
    selecting.fsm.loopSlotControlPressed(start_time);
    EXPECT_CALL(selecting.output, octaveUpLive());
    EXPECT_EQ(selecting.fsm.octaveUpPressed(), StateId::Ready);

    Harness selecting_next;
    selecting_next.fsm.loopSlotControlPressed(start_time);
    EXPECT_CALL(selecting_next.output, selectNextLiveSoundFont());
    EXPECT_EQ(selecting_next.fsm.nextSoundFontPressed(), StateId::Ready);

    Harness ready_soundfont_selecting;
    ready_soundfont_selecting.fsm.soundFontByNotePressed();
    EXPECT_CALL(ready_soundfont_selecting.output, octaveUpLive());
    EXPECT_EQ(
        ready_soundfont_selecting.fsm.octaveUpPressed(),
        StateId::Ready
    );

    Harness armed;
    armed.selectMutedSlot();
    EXPECT_CALL(armed.output, selectNextLoopSlotSoundFont(selected_slot));
    EXPECT_EQ(armed.fsm.nextSoundFontPressed(), StateId::Armed);
    EXPECT_CALL(armed.output, octaveDownLive());
    EXPECT_CALL(armed.output, octaveDownLoopSlot(selected_slot));
    EXPECT_EQ(armed.fsm.octaveDownPressed(), StateId::Armed);

    Harness armed_selecting;
    armed_selecting.selectMutedSlot();
    armed_selecting.fsm.soundFontByNotePressed();
    EXPECT_CALL(armed_selecting.output, octaveUpLive());
    EXPECT_CALL(
        armed_selecting.output,
        octaveUpLoopSlot(selected_slot)
    );
    EXPECT_EQ(armed_selecting.fsm.octaveUpPressed(), StateId::Armed);

    Harness armed_selecting_next;
    armed_selecting_next.selectMutedSlot();
    armed_selecting_next.fsm.soundFontByNotePressed();
    EXPECT_CALL(
        armed_selecting_next.output,
        selectNextLoopSlotSoundFont(selected_slot)
    );
    EXPECT_EQ(
        armed_selecting_next.fsm.nextSoundFontPressed(),
        StateId::Armed
    );

    Harness recording;
    recording.startRecording();
    EXPECT_EQ(recording.fsm.nextSoundFontPressed(), StateId::Recording);
    EXPECT_EQ(recording.fsm.soundFontByNotePressed(), StateId::Recording);
    EXPECT_EQ(recording.fsm.octaveDownPressed(), StateId::Recording);
    EXPECT_EQ(recording.fsm.octaveUpPressed(), StateId::Recording);
}

TEST(LooperFsmTest, ShutdownTerminatesSlotsThenSilencesFromEveryLiveState) {
    for (const StateId target : {
        StateId::Ready,
        StateId::ReadySelectingLoopSlot,
        StateId::ReadySelectingSoundFont,
        StateId::Armed,
        StateId::ArmedSelectingSoundFont,
        StateId::Recording,
    }) {
        Harness harness;
        if (target == StateId::ReadySelectingLoopSlot) {
            harness.fsm.loopSlotControlPressed(start_time);
        } else if (target == StateId::ReadySelectingSoundFont) {
            harness.fsm.soundFontByNotePressed();
        } else if (target == StateId::Armed) {
            harness.selectMutedSlot();
        } else if (target == StateId::ArmedSelectingSoundFont) {
            harness.selectMutedSlot();
            harness.fsm.soundFontByNotePressed();
        } else if (target == StateId::Recording) {
            harness.startRecording();
        }
        ASSERT_EQ(harness.fsm.stateId(), target);

        InSequence sequence;
        EXPECT_CALL(harness.output, terminateLoopSlots());
        EXPECT_CALL(harness.output, silenceAllChannels());
        EXPECT_EQ(harness.fsm.shutdownRequested(), StateId::Stopped);
        EXPECT_FALSE(harness.fsm.shouldRun());
        EXPECT_EQ(harness.fsm.shutdownRequested(), StateId::Stopped);
    }
}

TEST(LooperFsmTest, StoppedIsInertForEveryPerformerStimulus) {
    Harness harness;
    harness.fsm.shutdownRequested();

    EXPECT_EQ(
        harness.fsm.loopSlotControlPressed(start_time),
        StateId::Stopped
    );
    EXPECT_EQ(harness.fsm.nextSoundFontPressed(), StateId::Stopped);
    EXPECT_EQ(harness.fsm.soundFontByNotePressed(), StateId::Stopped);
    EXPECT_EQ(harness.fsm.octaveDownPressed(), StateId::Stopped);
    EXPECT_EQ(harness.fsm.octaveUpPressed(), StateId::Stopped);
    EXPECT_EQ(
        harness.fsm.midiMessage(
            MidiMessageType::NoteOn,
            noteOn(),
            start_time
        ),
        0
    );
}

} // namespace
