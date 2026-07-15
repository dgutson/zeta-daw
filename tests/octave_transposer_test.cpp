#include "../octave_transposer.hpp"

#include <gtest/gtest.h>
#include <hegel/hegel.h>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace {

namespace gs = hegel::generators;

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

void applyOctaveOperations(
    OctaveTransposer& transposer,
    const std::vector<bool>& octave_up_operations
) {
    for (const bool octave_up : octave_up_operations) {
        if (octave_up) {
            transposer.octaveUp();
        } else {
            transposer.octaveDown();
        }
    }
}

int modeledOctaves(const std::vector<bool>& octave_up_operations) {
    int octaves = 0;
    for (const bool octave_up : octave_up_operations) {
        if (octave_up && octaves < 4) {
            ++octaves;
        } else if (!octave_up && octaves > -3) {
            --octaves;
        }
    }
    return octaves;
}

MidiMessageType keyMessageType(int choice) {
    switch (choice) {
    case 0:
        return MidiMessageType::NoteOff;
    case 1:
        return MidiMessageType::NoteOn;
    default:
        return MidiMessageType::PolyphonicKeyPressure;
    }
}

MidiMessage generatedMessage(
    hegel::TestCase& tc,
    MidiMessageType type
) {
    return {
        .raw_type = raw(type),
        .channel = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 15})),
        .key = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 127})),
        .velocity = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 127})),
        .control = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 127})),
        .value = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 127})),
        .program = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 127})),
        .pitch = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 16383})),
        .pressure = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 127})),
        .device_id = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 127})),
        .machine_control_command =
            tc.draw(gs::integers<int>({.min_value = 0, .max_value = 127})),
    };
}

bool equalExceptKey(const MidiMessage& first, const MidiMessage& second) {
    return first.raw_type == second.raw_type
        && first.channel == second.channel
        && first.velocity == second.velocity
        && first.control == second.control
        && first.value == second.value
        && first.program == second.program
        && first.pitch == second.pitch
        && first.pressure == second.pressure
        && first.device_id == second.device_id
        && first.machine_control_command == second.machine_control_command;
}

bool equalMessage(const MidiMessage& first, const MidiMessage& second) {
    return first.key == second.key && equalExceptKey(first, second);
}

HEGEL_TEST(octave_sequence_matches_clamped_model)(hegel::TestCase& tc) {
    const auto operations = tc.draw(gs::vectors(gs::booleans()));
    const auto type = keyMessageType(
        tc.draw(gs::integers<int>({.min_value = 0, .max_value = 2}))
    );
    const auto message = generatedMessage(tc, type);

    OctaveTransposer transposer;
    applyOctaveOperations(transposer, operations);

    int expected_key = message.key + modeledOctaves(operations) * 12;
    if (expected_key < 0 || expected_key > 127) {
        expected_key = message.key;
    }

    if (transposer.transpose(message).key != expected_key) {
        throw std::runtime_error("octave transposition disagrees with model");
    }
}

HEGEL_TEST(octave_transposition_preserves_non_key_fields)(
    hegel::TestCase& tc
) {
    const auto operations = tc.draw(gs::vectors(gs::booleans()));
    const auto type = keyMessageType(
        tc.draw(gs::integers<int>({.min_value = 0, .max_value = 2}))
    );
    const auto message = generatedMessage(tc, type);

    OctaveTransposer transposer;
    applyOctaveOperations(transposer, operations);

    if (!equalExceptKey(transposer.transpose(message), message)) {
        throw std::runtime_error("octave transposition changed a non-key field");
    }
}

HEGEL_TEST(octave_transposition_preserves_non_key_messages)(
    hegel::TestCase& tc
) {
    constexpr std::array non_key_types{
        MidiMessageType::ControlChange,
        MidiMessageType::ProgramChange,
        MidiMessageType::ChannelPressure,
        MidiMessageType::PitchBend,
        MidiMessageType::MachineControl,
        MidiMessageType::Other,
    };
    const auto operations = tc.draw(gs::vectors(gs::booleans()));
    const auto type_index = tc.draw(gs::integers<std::size_t>({
        .min_value = 0,
        .max_value = non_key_types.size() - 1,
    }));
    const auto message = generatedMessage(tc, non_key_types[type_index]);

    OctaveTransposer transposer;
    applyOctaveOperations(transposer, operations);

    if (!equalMessage(transposer.transpose(message), message)) {
        throw std::runtime_error("octave transposition changed a non-key message");
    }
}

TEST(OctaveTransposerPropertyTest, SequenceMatchesClampedModel) {
    octave_sequence_matches_clamped_model();
}

TEST(OctaveTransposerPropertyTest, PreservesNonKeyFields) {
    octave_transposition_preserves_non_key_fields();
}

TEST(OctaveTransposerPropertyTest, PreservesNonKeyMessages) {
    octave_transposition_preserves_non_key_messages();
}

TEST(OctaveTransposerTest, StartsAtZeroAndMovesByTwelveSemitones) {
    OctaveTransposer transposer;

    EXPECT_EQ(transposer.transpose(noteOn(60)).key, 60);

    transposer.octaveUp();
    EXPECT_EQ(transposer.transpose(noteOn(60)).key, 72);

    transposer.octaveDown();
    transposer.octaveDown();
    EXPECT_EQ(transposer.transpose(noteOn(60)).key, 48);
}

TEST(OctaveTransposerTest, ClampsOctavesFromMinusThreeThroughPlusFour) {
    OctaveTransposer transposer;

    for (int press = 0; press < 5; ++press) {
        transposer.octaveUp();
    }
    EXPECT_EQ(transposer.transpose(noteOn(60)).key, 108);

    for (int press = 0; press < 8; ++press) {
        transposer.octaveDown();
    }
    EXPECT_EQ(transposer.transpose(noteOn(60)).key, 24);
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

    EXPECT_EQ(transposer.transpose(note_off).key, 72);
    EXPECT_EQ(transposer.transpose(key_pressure).key, 73);
    EXPECT_EQ(transposer.transpose(key_pressure).pressure, 90);
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

    EXPECT_EQ(transposed.control, 64);
    EXPECT_EQ(transposed.value, 127);
}

TEST(OctaveTransposerTest, LeavesKeysOutsideTheMidiRangeUnchanged) {
    OctaveTransposer transposer;
    transposer.octaveUp();

    EXPECT_EQ(transposer.transpose(noteOn(120)).key, 120);

    transposer.octaveDown();
    transposer.octaveDown();
    EXPECT_EQ(transposer.transpose(noteOn(0)).key, 0);
}

} // namespace
