#include "../octave_transposer.hpp"

#include <gtest/gtest.h>

namespace {

using zeta::MidiMessage;
using zeta::MidiMessageType;
using zeta::OctaveTransposer;

constexpr int raw(MidiMessageType type) {
    return static_cast<int>(type);
}

MidiMessage noteOn(int key) {
    return {
        .raw_type = raw(MidiMessageType::NoteOn),
        .key = key,
        .velocity = 100,
    };
}

TEST(OctaveTransposerTest, StartsAtZeroAndMovesByTwelveSemitones) {
    OctaveTransposer transposer;

    ASSERT_TRUE(transposer.transpose(noteOn(60)));
    EXPECT_EQ(transposer.transpose(noteOn(60))->key, 60);

    transposer.octaveUp();
    EXPECT_EQ(transposer.transpose(noteOn(60))->key, 72);

    transposer.octaveDown();
    transposer.octaveDown();
    EXPECT_EQ(transposer.transpose(noteOn(60))->key, 48);
}

TEST(OctaveTransposerTest, ClampsOctavesFromMinusThreeThroughPlusFour) {
    OctaveTransposer transposer;

    for (int press = 0; press < 5; ++press) {
        transposer.octaveUp();
    }
    EXPECT_EQ(transposer.transpose(noteOn(60))->key, 108);

    for (int press = 0; press < 8; ++press) {
        transposer.octaveDown();
    }
    EXPECT_EQ(transposer.transpose(noteOn(60))->key, 24);
}

TEST(OctaveTransposerTest, TransposesEveryKeyBearingMessage) {
    OctaveTransposer transposer;
    transposer.octaveUp();

    const MidiMessage note_off{
        .raw_type = raw(MidiMessageType::NoteOff),
        .key = 60,
    };
    const MidiMessage key_pressure{
        .raw_type = raw(MidiMessageType::PolyphonicKeyPressure),
        .key = 61,
        .pressure = 90,
    };

    ASSERT_TRUE(transposer.transpose(note_off));
    ASSERT_TRUE(transposer.transpose(key_pressure));
    EXPECT_EQ(transposer.transpose(note_off)->key, 72);
    EXPECT_EQ(transposer.transpose(key_pressure)->key, 73);
    EXPECT_EQ(transposer.transpose(key_pressure)->pressure, 90);
}

TEST(OctaveTransposerTest, LeavesMessagesWithoutKeysUnchanged) {
    OctaveTransposer transposer;
    transposer.octaveUp();
    const MidiMessage sustain{
        .raw_type = raw(MidiMessageType::ControlChange),
        .control = 64,
        .value = 127,
    };

    const auto transposed = transposer.transpose(sustain);

    ASSERT_TRUE(transposed);
    EXPECT_EQ(transposed->control, 64);
    EXPECT_EQ(transposed->value, 127);
}

TEST(OctaveTransposerTest, SuppressesKeysOutsideTheMidiRange) {
    OctaveTransposer transposer;
    transposer.octaveUp();

    EXPECT_FALSE(transposer.transpose(noteOn(120)));

    transposer.octaveDown();
    transposer.octaveDown();
    EXPECT_FALSE(transposer.transpose(noteOn(0)));
}

} // namespace
