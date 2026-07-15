#include "../midi_event.hpp"

#include <gtest/gtest.h>
#include <hegel/hegel.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {

namespace gs = hegel::generators;

using zeta::MidiMessageType;

struct ChannelMessageShape {
    std::uint8_t status;
    std::size_t data_byte_count;
};

constexpr std::array channel_message_shapes{
    ChannelMessageShape{0x8F, 2},
    ChannelMessageShape{0x9F, 2},
    ChannelMessageShape{0xAF, 2},
    ChannelMessageShape{0xBF, 2},
    ChannelMessageShape{0xCF, 1},
    ChannelMessageShape{0xDF, 1},
    ChannelMessageShape{0xEF, 2},
};

HEGEL_TEST(channel_voice_decoder_accepts_only_valid_data_bytes)(
    hegel::TestCase& tc
) {
    const auto shape_index = tc.draw(gs::integers<std::size_t>({
        .min_value = 0,
        .max_value = channel_message_shapes.size() - 1,
    }));
    const auto channel = tc.draw(gs::integers<std::uint8_t>({
        .min_value = 0,
        .max_value = 15,
    }));
    const auto first_data_byte = tc.draw(gs::integers<std::uint8_t>());
    const auto second_data_byte = tc.draw(gs::integers<std::uint8_t>());
    const auto& shape = channel_message_shapes[shape_index];
    const std::array bytes{
        static_cast<std::uint8_t>((shape.status & 0xF0) | channel),
        first_data_byte,
        second_data_byte,
    };

    const auto event = zeta::decodeMidiEvent(
        std::span{bytes}.first(shape.data_byte_count + 1)
    );
    const bool data_bytes_are_valid = first_data_byte < 0x80
        && (shape.data_byte_count == 1 || second_data_byte < 0x80);
    if (event.has_value() != data_bytes_are_valid) {
        throw std::runtime_error(
            "channel voice decoder violated the MIDI data-byte domain"
        );
    }
}

TEST(MidiDecoderPropertyTest, AcceptsOnlyValidChannelVoiceDataBytes) {
    channel_voice_decoder_accepts_only_valid_data_bytes();
}

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

TEST(MidiTest, RejectsMalformedControlChangeBeforeMapping) {
    constexpr std::array<std::uint8_t, 3> malformed_control_change{
        0xBF, 0xFF, 0x00,
    };

    EXPECT_FALSE(zeta::decodeMidiEvent(malformed_control_change));
}

TEST(MidiTest, AcceptsChannelVoiceDataByteBoundaries) {
    for (const auto& shape : channel_message_shapes) {
        constexpr std::array boundaries{
            std::uint8_t{0},
            std::uint8_t{0x7F},
        };
        for (const auto boundary : boundaries) {
            const std::array bytes{shape.status, boundary, boundary};

            const auto event = zeta::decodeMidiEvent(
                std::span{bytes}.first(shape.data_byte_count + 1)
            );

            SCOPED_TRACE(static_cast<int>(shape.status));
            SCOPED_TRACE(static_cast<int>(boundary));
            EXPECT_TRUE(event);
        }
    }
}

TEST(MidiTest, RejectsOutOfDomainBytesInEveryChannelVoiceDataPosition) {
    for (const auto& shape : channel_message_shapes) {
        for (
            std::size_t position = 1;
            position <= shape.data_byte_count;
            ++position
        ) {
            std::array<std::uint8_t, 3> bytes{shape.status, 0, 0};
            bytes[position] = 0x80;

            const auto event = zeta::decodeMidiEvent(
                std::span{bytes}.first(shape.data_byte_count + 1)
            );

            SCOPED_TRACE(static_cast<int>(shape.status));
            SCOPED_TRACE(position);
            EXPECT_FALSE(event);
        }
    }
}

} // namespace
