#include "../midi_control_change_mapping.hpp"

#include <gtest/gtest.h>

#include <array>

namespace {

using zeta::MidiControlChangeMapper;
using zeta::MidiControlChangeMapping;
using zeta::MidiEvent;
using zeta::MidiMessageType;

constexpr int raw(MidiMessageType type) noexcept {
    return static_cast<int>(type);
}

MidiEvent controlChange(int channel, int controller, int value) {
    return {
        .type = MidiMessageType::ControlChange,
        .message = {
            .raw_type = raw(MidiMessageType::ControlChange),
            .channel = channel,
            .control = controller,
            .value = value,
        },
    };
}

TEST(MidiControlChangeMapperTest, MapsExactSourceChannelAndController) {
    const std::array mappings{
        MidiControlChangeMapping{
            .source_port = "Controller MIDI2",
            .channel = 15,
            .controller = 20,
            .target_controller = 7,
        },
    };
    const MidiControlChangeMapper mapper{"Controller MIDI2", mappings};

    const auto mapped = mapper.map(controlChange(15, 20, 83));

    EXPECT_EQ(mapped.type, MidiMessageType::ControlChange);
    EXPECT_EQ(mapped.message.raw_type, raw(MidiMessageType::ControlChange));
    EXPECT_EQ(mapped.message.channel, 15);
    EXPECT_EQ(mapped.message.control, 7);
    EXPECT_EQ(mapped.message.value, 83);
}

TEST(MidiControlChangeMapperTest, LeavesNonmatchingEventsUnchanged) {
    const std::array mappings{
        MidiControlChangeMapping{
            .source_port = "Controller MIDI2",
            .channel = 15,
            .controller = 20,
            .target_controller = 7,
        },
    };
    const MidiControlChangeMapper wrong_source{"Controller MIDI1", mappings};
    const MidiControlChangeMapper matching_source{"Controller MIDI2", mappings};

    EXPECT_EQ(
        wrong_source.map(controlChange(15, 20, 64)).message.control,
        20
    );
    EXPECT_EQ(
        matching_source.map(controlChange(14, 20, 64)).message.control,
        20
    );
    EXPECT_EQ(
        matching_source.map(controlChange(15, 21, 64)).message.control,
        21
    );

    constexpr std::array non_control_change_types{
        MidiMessageType::NoteOff,
        MidiMessageType::NoteOn,
        MidiMessageType::PolyphonicKeyPressure,
        MidiMessageType::ProgramChange,
        MidiMessageType::ChannelPressure,
        MidiMessageType::PitchBend,
        MidiMessageType::MachineControl,
        MidiMessageType::Other,
    };
    for (const auto type : non_control_change_types) {
        const MidiEvent event{
            .type = type,
            .message = {
                .raw_type = raw(type),
                .channel = 15,
                .key = 20,
                .velocity = 100,
                .control = 20,
                .value = 64,
            },
        };

        const auto unchanged = matching_source.map(event);

        SCOPED_TRACE(static_cast<int>(type));
        EXPECT_EQ(unchanged.type, type);
        EXPECT_EQ(unchanged.message.raw_type, raw(type));
        EXPECT_EQ(unchanged.message.key, 20);
        EXPECT_EQ(unchanged.message.control, 20);
        EXPECT_EQ(unchanged.message.value, 64);
    }
}

TEST(MidiControlChangeMapperTest, AppliesMappingsOnceWithoutChaining) {
    const std::array mappings{
        MidiControlChangeMapping{
            .source_port = "Controller MIDI2",
            .channel = 15,
            .controller = 20,
            .target_controller = 7,
        },
        MidiControlChangeMapping{
            .source_port = "Controller MIDI2",
            .channel = 15,
            .controller = 7,
            .target_controller = 11,
        },
    };
    const MidiControlChangeMapper mapper{"Controller MIDI2", mappings};

    EXPECT_EQ(mapper.map(controlChange(15, 20, 64)).message.control, 7);
    EXPECT_EQ(mapper.map(controlChange(15, 7, 64)).message.control, 11);
}

TEST(MidiControlChangeMapperTest, SupportsSeveralMappingsIndependently) {
    const std::array mappings{
        MidiControlChangeMapping{
            .source_port = "Controller MIDI2",
            .channel = 15,
            .controller = 20,
            .target_controller = 7,
        },
        MidiControlChangeMapping{
            .source_port = "Controller MIDI2",
            .channel = 0,
            .controller = 21,
            .target_controller = 1,
        },
        MidiControlChangeMapping{
            .source_port = "Other Controller",
            .channel = 15,
            .controller = 20,
            .target_controller = 74,
        },
    };
    const MidiControlChangeMapper mapper{"Controller MIDI2", mappings};

    EXPECT_EQ(mapper.map(controlChange(15, 20, 1)).message.control, 7);
    EXPECT_EQ(mapper.map(controlChange(0, 21, 2)).message.control, 1);
}

} // namespace
