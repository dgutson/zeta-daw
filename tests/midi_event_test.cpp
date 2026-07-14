#include "../midi_event.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using zeta::MidiMessageType;

TEST(MidiTest, DecodesChannelVoiceMessages) {
    constexpr std::array<std::uint8_t, 3> note{0x92, 64, 91};
    const auto event = zeta::decodeMidiEvent(note);

    ASSERT_TRUE(event);
    EXPECT_EQ(event->type, MidiMessageType::NoteOn);
    EXPECT_EQ(event->message.channel, 2);
    EXPECT_EQ(event->message.key, 64);
    EXPECT_EQ(event->message.velocity, 91);

    constexpr std::array<std::uint8_t, 3> bend{0xE0, 0x7F, 0x7F};
    const auto bend_event = zeta::decodeMidiEvent(bend);

    ASSERT_TRUE(bend_event);
    EXPECT_EQ(bend_event->type, MidiMessageType::PitchBend);
    EXPECT_EQ(bend_event->message.pitch, 16383);
}

TEST(MidiTest, DecodesMachineControlFromUniversalRealtimeSysEx) {
    constexpr std::array<std::uint8_t, 6> stop{
        0xF0, 0x7F, 0x10, 0x06, 0x01, 0xF7,
    };
    const auto event = zeta::decodeMidiEvent(stop);

    ASSERT_TRUE(event);
    EXPECT_EQ(event->type, MidiMessageType::MachineControl);
    EXPECT_EQ(event->message.device_id, 0x10);
    EXPECT_EQ(event->message.machine_control_command, 0x01);
}

TEST(MidiTest, DecodesSe49OctaveMachineControlCommands) {
    constexpr std::array commands{
        std::uint8_t{0x02},
        std::uint8_t{0x06},
    };
    for (const std::uint8_t command : commands) {
        const std::array<std::uint8_t, 6> bytes{
            0xF0, 0x7F, 0x10, 0x06, command, 0xF7,
        };

        const auto event = zeta::decodeMidiEvent(bytes);

        ASSERT_TRUE(event);
        EXPECT_EQ(event->type, MidiMessageType::MachineControl);
        EXPECT_EQ(event->message.machine_control_command, command);
    }
}

TEST(MidiTest, DoesNotMisclassifyOtherOrMalformedSysExAsMachineControl) {
    constexpr std::array<std::uint8_t, 6> non_mmc{
        0xF0, 0x7E, 0x10, 0x06, 0x01, 0xF7,
    };
    const auto event = zeta::decodeMidiEvent(non_mmc);

    ASSERT_TRUE(event);
    EXPECT_EQ(event->type, MidiMessageType::Other);

    constexpr std::array<std::uint8_t, 2> truncated_note{0x90, 60};
    EXPECT_FALSE(zeta::decodeMidiEvent(truncated_note));
}

} // namespace
