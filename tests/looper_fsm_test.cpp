#include "../looper_fsm.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>

namespace {

using namespace std::chrono_literals;
using ::testing::_;
using ::testing::InSequence;
using ::testing::Return;
using ::testing::StrictMock;
using zeta::LooperFsm;
using zeta::LooperOutput;
using zeta::LooperStateRegistry;
using zeta::Milliseconds;
using zeta::MidiMessage;
using zeta::MidiMessageType;
using zeta::MidiRoute;
using zeta::RecordedNoteKind;
using zeta::StateId;
using zeta::TimePoint;

class MockOutput : public LooperOutput {
public:
    MOCK_METHOD(int, monitorMidi, (const MidiMessage&, MidiRoute), (override));
    MOCK_METHOD(void, selectCurrentSoundFont, (MidiRoute), (override));
    MOCK_METHOD(void, selectNextSoundFont, (MidiRoute), (override));
    MOCK_METHOD(
        void,
        selectSoundFontByNote,
        (MidiRoute, int, int),
        (override)
    );
    MOCK_METHOD(void, octaveDown, (MidiRoute), (override));
    MOCK_METHOD(void, octaveUp, (MidiRoute), (override));
    MOCK_METHOD(void, stopLoopPlayback, (), (override));
    MOCK_METHOD(void, silenceAllChannels, (), (override));
    MOCK_METHOD(void, resetTake, (), (override));
    MOCK_METHOD(
        void,
        recordNote,
        (RecordedNoteKind, const MidiMessage&, Milliseconds),
        (override)
    );
    MOCK_METHOD(void, commitTake, (Milliseconds), (override));
    MOCK_METHOD(void, startLoopPlayback, (), (override));
    MOCK_METHOD(void, showRecordingArmed, (), (override));
    MOCK_METHOD(void, showLooping, (), (override));
    MOCK_METHOD(void, showNoTake, (), (override));
    MOCK_METHOD(void, stopPlaybackWorker, (), (override));
};

constexpr auto start_time = TimePoint{} + 100ms;

void expectArm(StrictMock<MockOutput>& output) {
    InSequence sequence;
    EXPECT_CALL(output, stopLoopPlayback());
    EXPECT_CALL(output, silenceAllChannels());
    EXPECT_CALL(output, selectCurrentSoundFont(MidiRoute::LoopChannel));
    EXPECT_CALL(output, resetTake());
    EXPECT_CALL(output, showRecordingArmed());
}

void arm(LooperFsm& fsm, StrictMock<MockOutput>& output) {
    expectArm(output);
    EXPECT_EQ(fsm.recordingControlPressed(start_time), StateId::Armed);
    EXPECT_EQ(fsm.stateId(), StateId::Armed);
}

void startLooping(LooperFsm& fsm, StrictMock<MockOutput>& output) {
    arm(fsm, output);
    MidiMessage note_on{.channel = 0, .key = 60, .velocity = 100};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(0));
    EXPECT_CALL(output, recordNote(RecordedNoteKind::NoteOn, _, 0ms));
    fsm.midiMessage(MidiMessageType::NoteOn, note_on, start_time);

    {
        InSequence sequence;
        EXPECT_CALL(output, commitTake(1ms));
        EXPECT_CALL(output, startLoopPlayback());
        EXPECT_CALL(output, showLooping());
    }
    EXPECT_EQ(fsm.recordingControlPressed(start_time + 1ms), StateId::Looping);
}

TEST(LooperFsmTest, ReadyMonitorsMidiOnTheDedicatedLiveChannel) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};
    MidiMessage message{.channel = 7, .key = 64, .velocity = 90};

    EXPECT_EQ(fsm.stateId(), StateId::Ready);
    EXPECT_TRUE(fsm.shouldRun());
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LiveChannel))
        .WillOnce(Return(23));

    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, message, start_time),
        23
    );
    EXPECT_EQ(fsm.stateId(), StateId::Ready);
}

TEST(LooperFsmTest, NextSoundFontUsesStateSpecificRouteAndIsIgnoredRecording) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};

    EXPECT_CALL(output, selectNextSoundFont(MidiRoute::LiveChannel));
    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Ready);

    arm(fsm, output);
    EXPECT_CALL(output, selectNextSoundFont(MidiRoute::LoopChannel));
    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Armed);

    MidiMessage note_on{.channel = 0, .key = 60, .velocity = 100};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel))
        .WillOnce(Return(0));
    EXPECT_CALL(output, recordNote(RecordedNoteKind::NoteOn, _, 0ms));
    fsm.midiMessage(MidiMessageType::NoteOn, note_on, start_time + 1ms);

    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Recording);

    {
        InSequence sequence;
        EXPECT_CALL(output, commitTake(9ms));
        EXPECT_CALL(output, startLoopPlayback());
        EXPECT_CALL(output, showLooping());
    }
    EXPECT_EQ(fsm.recordingControlPressed(start_time + 10ms), StateId::Looping);

    EXPECT_CALL(output, selectNextSoundFont(MidiRoute::LiveChannel));
    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Looping);
}

TEST(LooperFsmTest, SoundFontByNoteUsesExplicitStateAndStateSpecificRoute) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};
    MidiMessage selection_note{.channel = 2, .key = 60, .velocity = 100};

    EXPECT_EQ(
        fsm.soundFontByNotePressed(),
        StateId::ReadySelectingSoundFont
    );
    EXPECT_CALL(
        output,
        selectSoundFontByNote(MidiRoute::LiveChannel, 2, 60)
    );
    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, selection_note, start_time),
        0
    );
    EXPECT_EQ(fsm.stateId(), StateId::Ready);

    arm(fsm, output);
    EXPECT_EQ(
        fsm.soundFontByNotePressed(),
        StateId::ArmedSelectingSoundFont
    );
    EXPECT_CALL(
        output,
        selectSoundFontByNote(MidiRoute::LoopChannel, 2, 60)
    );
    fsm.midiMessage(MidiMessageType::NoteOn, selection_note, start_time + 1ms);
    EXPECT_EQ(fsm.stateId(), StateId::Armed);

    MidiMessage recording_note{.channel = 0, .key = 67, .velocity = 90};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(5));
    EXPECT_CALL(output, recordNote(RecordedNoteKind::NoteOn, _, 0ms));
    EXPECT_EQ(
        fsm.midiMessage(
            MidiMessageType::NoteOn,
            recording_note,
            start_time + 2ms
        ),
        5
    );
    EXPECT_EQ(fsm.stateId(), StateId::Recording);
    EXPECT_EQ(fsm.soundFontByNotePressed(), StateId::Recording);

    {
        InSequence sequence;
        EXPECT_CALL(output, commitTake(8ms));
        EXPECT_CALL(output, startLoopPlayback());
        EXPECT_CALL(output, showLooping());
    }
    fsm.recordingControlPressed(start_time + 10ms);

    EXPECT_EQ(
        fsm.soundFontByNotePressed(),
        StateId::LoopingSelectingSoundFont
    );
    EXPECT_CALL(
        output,
        selectSoundFontByNote(MidiRoute::LiveChannel, 2, 60)
    );
    fsm.midiMessage(MidiMessageType::NoteOn, selection_note, start_time + 11ms);
    EXPECT_EQ(fsm.stateId(), StateId::Looping);
}

TEST(LooperFsmTest, SoundFontSelectionStatesRouteOtherMidiAndCancel) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};
    MidiMessage message{.channel = 0, .key = 60, .velocity = 0};

    fsm.soundFontByNotePressed();
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LiveChannel)).WillOnce(Return(3));
    EXPECT_EQ(fsm.midiMessage(MidiMessageType::NoteOn, message, start_time), 3);
    EXPECT_EQ(fsm.stateId(), StateId::ReadySelectingSoundFont);
    EXPECT_EQ(fsm.soundFontByNotePressed(), StateId::Ready);

    arm(fsm, output);
    fsm.soundFontByNotePressed();
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(4));
    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, message, start_time + 1ms),
        4
    );
    EXPECT_EQ(fsm.stateId(), StateId::ArmedSelectingSoundFont);
    EXPECT_EQ(fsm.soundFontByNotePressed(), StateId::Armed);

    MidiMessage recording_note{.channel = 0, .key = 64, .velocity = 100};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(0));
    EXPECT_CALL(output, recordNote(RecordedNoteKind::NoteOn, _, 0ms));
    fsm.midiMessage(MidiMessageType::NoteOn, recording_note, start_time + 2ms);
    {
        InSequence sequence;
        EXPECT_CALL(output, commitTake(1ms));
        EXPECT_CALL(output, startLoopPlayback());
        EXPECT_CALL(output, showLooping());
    }
    fsm.recordingControlPressed(start_time + 3ms);

    fsm.soundFontByNotePressed();
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LiveChannel)).WillOnce(Return(6));
    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, message, start_time + 4ms),
        6
    );
    EXPECT_EQ(fsm.stateId(), StateId::LoopingSelectingSoundFont);
    EXPECT_EQ(fsm.soundFontByNotePressed(), StateId::Looping);
}

TEST(LooperFsmTest, OtherControlsLeaveSoundFontSelectionStates) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};

    fsm.soundFontByNotePressed();
    EXPECT_CALL(output, selectNextSoundFont(MidiRoute::LiveChannel));
    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Ready);
    fsm.soundFontByNotePressed();
    {
        InSequence sequence;
        EXPECT_CALL(output, octaveDown(MidiRoute::LiveChannel));
        EXPECT_CALL(output, octaveDown(MidiRoute::LoopChannel));
    }
    EXPECT_EQ(fsm.octaveDownPressed(), StateId::Ready);
    fsm.soundFontByNotePressed();
    {
        InSequence sequence;
        EXPECT_CALL(output, octaveUp(MidiRoute::LiveChannel));
        EXPECT_CALL(output, octaveUp(MidiRoute::LoopChannel));
    }
    EXPECT_EQ(fsm.octaveUpPressed(), StateId::Ready);

    fsm.soundFontByNotePressed();
    expectArm(output);
    EXPECT_EQ(fsm.recordingControlPressed(start_time), StateId::Armed);
    fsm.soundFontByNotePressed();
    EXPECT_CALL(output, selectNextSoundFont(MidiRoute::LoopChannel));
    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Armed);
    fsm.soundFontByNotePressed();
    {
        InSequence sequence;
        EXPECT_CALL(output, octaveDown(MidiRoute::LiveChannel));
        EXPECT_CALL(output, octaveDown(MidiRoute::LoopChannel));
    }
    EXPECT_EQ(fsm.octaveDownPressed(), StateId::Armed);
    fsm.soundFontByNotePressed();
    {
        InSequence sequence;
        EXPECT_CALL(output, octaveUp(MidiRoute::LiveChannel));
        EXPECT_CALL(output, octaveUp(MidiRoute::LoopChannel));
    }
    EXPECT_EQ(fsm.octaveUpPressed(), StateId::Armed);

    fsm.soundFontByNotePressed();
    {
        InSequence sequence;
        EXPECT_CALL(output, showNoTake());
        EXPECT_CALL(output, stopLoopPlayback());
        EXPECT_CALL(output, silenceAllChannels());
        EXPECT_CALL(output, selectCurrentSoundFont(MidiRoute::LiveChannel));
    }
    EXPECT_EQ(fsm.recordingControlPressed(start_time), StateId::Ready);

    startLooping(fsm, output);
    fsm.soundFontByNotePressed();
    EXPECT_CALL(output, selectNextSoundFont(MidiRoute::LiveChannel));
    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Looping);
    fsm.soundFontByNotePressed();
    EXPECT_CALL(output, octaveDown(MidiRoute::LiveChannel));
    EXPECT_EQ(fsm.octaveDownPressed(), StateId::Looping);
    fsm.soundFontByNotePressed();
    EXPECT_CALL(output, octaveUp(MidiRoute::LiveChannel));
    EXPECT_EQ(fsm.octaveUpPressed(), StateId::Looping);

    fsm.soundFontByNotePressed();
    {
        InSequence sequence;
        EXPECT_CALL(output, stopLoopPlayback());
        EXPECT_CALL(output, silenceAllChannels());
    }
    EXPECT_EQ(fsm.recordingControlPressed(start_time + 2ms), StateId::Ready);
}

TEST(LooperFsmTest, ShutdownFromEverySoundFontSelectionStateIsTerminal) {
    {
        StrictMock<MockOutput> output;
        LooperStateRegistry states{output};
        LooperFsm fsm{states};
        fsm.soundFontByNotePressed();
        {
            InSequence sequence;
            EXPECT_CALL(output, stopLoopPlayback());
            EXPECT_CALL(output, silenceAllChannels());
            EXPECT_CALL(output, stopPlaybackWorker());
        }
        EXPECT_EQ(fsm.shutdownRequested(), StateId::Stopped);
    }

    {
        StrictMock<MockOutput> output;
        LooperStateRegistry states{output};
        LooperFsm fsm{states};
        arm(fsm, output);
        fsm.soundFontByNotePressed();
        {
            InSequence sequence;
            EXPECT_CALL(output, stopLoopPlayback());
            EXPECT_CALL(output, silenceAllChannels());
            EXPECT_CALL(output, stopPlaybackWorker());
        }
        EXPECT_EQ(fsm.shutdownRequested(), StateId::Stopped);
    }

    {
        StrictMock<MockOutput> output;
        LooperStateRegistry states{output};
        LooperFsm fsm{states};
        startLooping(fsm, output);
        fsm.soundFontByNotePressed();
        {
            InSequence sequence;
            EXPECT_CALL(output, stopLoopPlayback());
            EXPECT_CALL(output, silenceAllChannels());
            EXPECT_CALL(output, stopPlaybackWorker());
        }
        EXPECT_EQ(fsm.shutdownRequested(), StateId::Stopped);
    }
}

TEST(LooperFsmTest, OctaveControlsUseStateSpecificRoutes) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};

    {
        InSequence sequence;
        EXPECT_CALL(output, octaveDown(MidiRoute::LiveChannel));
        EXPECT_CALL(output, octaveDown(MidiRoute::LoopChannel));
    }
    EXPECT_EQ(fsm.octaveDownPressed(), StateId::Ready);
    {
        InSequence sequence;
        EXPECT_CALL(output, octaveUp(MidiRoute::LiveChannel));
        EXPECT_CALL(output, octaveUp(MidiRoute::LoopChannel));
    }
    EXPECT_EQ(fsm.octaveUpPressed(), StateId::Ready);

    arm(fsm, output);
    {
        InSequence sequence;
        EXPECT_CALL(output, octaveDown(MidiRoute::LiveChannel));
        EXPECT_CALL(output, octaveDown(MidiRoute::LoopChannel));
    }
    EXPECT_EQ(fsm.octaveDownPressed(), StateId::Armed);
    {
        InSequence sequence;
        EXPECT_CALL(output, octaveUp(MidiRoute::LiveChannel));
        EXPECT_CALL(output, octaveUp(MidiRoute::LoopChannel));
    }
    EXPECT_EQ(fsm.octaveUpPressed(), StateId::Armed);

    MidiMessage note_on{.channel = 0, .key = 60, .velocity = 100};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(0));
    EXPECT_CALL(output, recordNote(RecordedNoteKind::NoteOn, _, 0ms));
    fsm.midiMessage(MidiMessageType::NoteOn, note_on, start_time);

    EXPECT_EQ(fsm.octaveDownPressed(), StateId::Recording);
    EXPECT_EQ(fsm.octaveUpPressed(), StateId::Recording);

    {
        InSequence sequence;
        EXPECT_CALL(output, commitTake(1ms));
        EXPECT_CALL(output, startLoopPlayback());
        EXPECT_CALL(output, showLooping());
    }
    fsm.recordingControlPressed(start_time + 1ms);

    EXPECT_CALL(output, octaveDown(MidiRoute::LiveChannel));
    EXPECT_EQ(fsm.octaveDownPressed(), StateId::Looping);
    EXPECT_CALL(output, octaveUp(MidiRoute::LiveChannel));
    EXPECT_EQ(fsm.octaveUpPressed(), StateId::Looping);

    {
        InSequence sequence;
        EXPECT_CALL(output, stopLoopPlayback());
        EXPECT_CALL(output, silenceAllChannels());
    }
    EXPECT_EQ(
        fsm.recordingControlPressed(start_time + 2ms),
        StateId::Ready
    );

    {
        InSequence sequence;
        EXPECT_CALL(output, octaveDown(MidiRoute::LiveChannel));
        EXPECT_CALL(output, octaveDown(MidiRoute::LoopChannel));
    }
    EXPECT_EQ(fsm.octaveDownPressed(), StateId::Ready);
    {
        InSequence sequence;
        EXPECT_CALL(output, octaveUp(MidiRoute::LiveChannel));
        EXPECT_CALL(output, octaveUp(MidiRoute::LoopChannel));
    }
    EXPECT_EQ(fsm.octaveUpPressed(), StateId::Ready);
}

TEST(LooperFsmTest, FirstPositiveVelocityNoteOnMovesArmedToRecording) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};
    arm(fsm, output);

    MidiMessage note_off{.channel = 0, .key = 60, .velocity = 0};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(0));
    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOff, note_off, start_time + 1ms),
        0
    );
    EXPECT_EQ(fsm.stateId(), StateId::Armed);

    MidiMessage zero_velocity_note_on{.channel = 0, .key = 60, .velocity = 0};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(0));
    EXPECT_EQ(
        fsm.midiMessage(
            MidiMessageType::NoteOn,
            zero_velocity_note_on,
            start_time + 2ms
        ),
        0
    );
    EXPECT_EQ(fsm.stateId(), StateId::Armed);

    MidiMessage note_on{.channel = 0, .key = 60, .velocity = 100};
    {
        InSequence sequence;
        EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(7));
        EXPECT_CALL(output, recordNote(RecordedNoteKind::NoteOn, _, 0ms));
    }

    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, note_on, start_time + 3ms),
        7
    );
    EXPECT_EQ(fsm.stateId(), StateId::Recording);
}

TEST(LooperFsmTest, CompletedRecordingCanStopBackToReady) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};
    arm(fsm, output);

    MidiMessage note_on{.channel = 0, .key = 60, .velocity = 100};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(0));
    EXPECT_CALL(output, recordNote(RecordedNoteKind::NoteOn, _, 0ms));
    fsm.midiMessage(MidiMessageType::NoteOn, note_on, start_time + 1ms);

    MidiMessage note_off{.channel = 0, .key = 60};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(0));
    EXPECT_CALL(output, recordNote(RecordedNoteKind::NoteOff, _, 19ms));
    fsm.midiMessage(MidiMessageType::NoteOff, note_off, start_time + 20ms);

    {
        InSequence sequence;
        EXPECT_CALL(output, commitTake(29ms));
        EXPECT_CALL(output, startLoopPlayback());
        EXPECT_CALL(output, showLooping());
    }
    EXPECT_EQ(
        fsm.recordingControlPressed(start_time + 30ms),
        StateId::Looping
    );
    EXPECT_EQ(fsm.stateId(), StateId::Looping);

    MidiMessage live_note{.channel = 0, .key = 72, .velocity = 80};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LiveChannel)).WillOnce(Return(11));
    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, live_note, start_time + 31ms),
        11
    );

    {
        InSequence sequence;
        EXPECT_CALL(output, stopLoopPlayback());
        EXPECT_CALL(output, silenceAllChannels());
    }
    EXPECT_EQ(
        fsm.recordingControlPressed(start_time + 32ms),
        StateId::Ready
    );
    EXPECT_EQ(fsm.stateId(), StateId::Ready);
    EXPECT_TRUE(fsm.shouldRun());

    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LiveChannel))
        .WillOnce(Return(13));
    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, live_note, start_time + 33ms),
        13
    );
}

TEST(LooperFsmTest, PressingControlWhileArmedCancelsBackToReady) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};
    arm(fsm, output);

    {
        InSequence sequence;
        EXPECT_CALL(output, showNoTake());
        EXPECT_CALL(output, stopLoopPlayback());
        EXPECT_CALL(output, silenceAllChannels());
        EXPECT_CALL(output, selectCurrentSoundFont(MidiRoute::LiveChannel));
    }

    EXPECT_EQ(
        fsm.recordingControlPressed(start_time + 1ms),
        StateId::Ready
    );
    EXPECT_EQ(fsm.stateId(), StateId::Ready);
    EXPECT_TRUE(fsm.shouldRun());

    MidiMessage live_note{.channel = 0, .key = 67, .velocity = 80};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LiveChannel))
        .WillOnce(Return(17));
    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, live_note, start_time + 2ms),
        17
    );
}

TEST(LooperFsmTest, ZeroDurationTakeUsesTheNormalRecordingTransition) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};
    arm(fsm, output);

    MidiMessage note_on{.channel = 0, .key = 60, .velocity = 100};
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(0));
    EXPECT_CALL(output, recordNote(RecordedNoteKind::NoteOn, _, 0ms));
    fsm.midiMessage(MidiMessageType::NoteOn, note_on, start_time);

    {
        InSequence sequence;
        EXPECT_CALL(output, commitTake(0ms));
        EXPECT_CALL(output, startLoopPlayback());
        EXPECT_CALL(output, showLooping());
    }

    EXPECT_EQ(fsm.recordingControlPressed(start_time), StateId::Looping);
    EXPECT_EQ(fsm.stateId(), StateId::Looping);
}

TEST(LooperFsmTest, ShutdownFromReadyIsTerminalAndIdempotent) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};

    {
        InSequence sequence;
        EXPECT_CALL(output, stopLoopPlayback());
        EXPECT_CALL(output, silenceAllChannels());
        EXPECT_CALL(output, stopPlaybackWorker());
    }
    fsm.shutdownRequested();
    EXPECT_EQ(fsm.stateId(), StateId::Stopped);
    EXPECT_FALSE(fsm.shouldRun());

    fsm.shutdownRequested();
    EXPECT_EQ(
        fsm.recordingControlPressed(start_time),
        StateId::Stopped
    );
    EXPECT_EQ(fsm.soundFontByNotePressed(), StateId::Stopped);
}

TEST(LooperFsmTest, ClassifiesKnownAndUnknownRawMidiTypes) {
    EXPECT_EQ(zeta::classifyMidiMessage(0x80), MidiMessageType::NoteOff);
    EXPECT_EQ(zeta::classifyMidiMessage(0x90), MidiMessageType::NoteOn);
    EXPECT_EQ(zeta::classifyMidiMessage(0xB0), MidiMessageType::ControlChange);
    EXPECT_EQ(zeta::classifyMidiMessage(0xC0), MidiMessageType::ProgramChange);
    EXPECT_EQ(zeta::classifyMidiMessage(0xE0), MidiMessageType::PitchBend);
    EXPECT_EQ(zeta::classifyMidiMessage(0xF8), MidiMessageType::Other);
}

} // namespace
