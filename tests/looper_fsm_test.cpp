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
    EXPECT_EQ(fsm.primaryControlPressed(start_time), StateId::Armed);
    EXPECT_EQ(fsm.stateId(), StateId::Armed);
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
    EXPECT_CALL(output, monitorMidi(_, MidiRoute::LoopChannel)).WillOnce(Return(0));
    EXPECT_CALL(output, recordNote(RecordedNoteKind::NoteOn, _, 0ms));
    fsm.midiMessage(MidiMessageType::NoteOn, note_on, start_time + 1ms);

    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Recording);

    {
        InSequence sequence;
        EXPECT_CALL(output, commitTake(9ms));
        EXPECT_CALL(output, startLoopPlayback());
        EXPECT_CALL(output, showLooping());
    }
    EXPECT_EQ(fsm.primaryControlPressed(start_time + 10ms), StateId::Looping);

    EXPECT_CALL(output, selectNextSoundFont(MidiRoute::LiveChannel));
    EXPECT_EQ(fsm.nextSoundFontPressed(), StateId::Looping);
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

TEST(LooperFsmTest, CompletedRecordingLoopsAndReturnsMidiToLiveChannel) {
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
        fsm.primaryControlPressed(start_time + 30ms),
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
        EXPECT_CALL(output, stopPlaybackWorker());
    }
    EXPECT_EQ(
        fsm.primaryControlPressed(start_time + 32ms),
        StateId::Stopped
    );
    EXPECT_EQ(fsm.stateId(), StateId::Stopped);

    EXPECT_EQ(
        fsm.midiMessage(MidiMessageType::NoteOn, live_note, start_time + 33ms),
        0
    );
}

TEST(LooperFsmTest, PressingControlWhileArmedReportsNoTakeAndStops) {
    StrictMock<MockOutput> output;
    LooperStateRegistry states{output};
    LooperFsm fsm{states};
    arm(fsm, output);

    {
        InSequence sequence;
        EXPECT_CALL(output, showNoTake());
        EXPECT_CALL(output, stopLoopPlayback());
        EXPECT_CALL(output, silenceAllChannels());
        EXPECT_CALL(output, stopPlaybackWorker());
    }

    EXPECT_EQ(
        fsm.primaryControlPressed(start_time + 1ms),
        StateId::Stopped
    );
    EXPECT_EQ(fsm.stateId(), StateId::Stopped);
}

TEST(LooperFsmTest, ZeroDurationTakeReportsNoTakeAndStops) {
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
        EXPECT_CALL(output, showNoTake());
        EXPECT_CALL(output, stopLoopPlayback());
        EXPECT_CALL(output, silenceAllChannels());
        EXPECT_CALL(output, stopPlaybackWorker());
    }

    EXPECT_EQ(fsm.primaryControlPressed(start_time), StateId::Stopped);
    EXPECT_EQ(fsm.stateId(), StateId::Stopped);
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
        fsm.primaryControlPressed(start_time),
        StateId::Stopped
    );
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
