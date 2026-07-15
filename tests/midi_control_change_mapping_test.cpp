#include "../midi_control_change_mapping.hpp"

#include <gtest/gtest.h>
#include <hegel/hegel.h>

#include <array>
#include <stdexcept>
#include <vector>

namespace {

namespace gs = hegel::generators;

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

HEGEL_TEST(control_change_mapping_matches_linear_model)(
    hegel::TestCase& tc
) {
    constexpr int channel_count = 16;
    constexpr int controller_count = 128;
    constexpr int source_count = 2;
    constexpr int mappings_per_source = channel_count * controller_count;

    const auto mapping_keys = tc.draw(gs::vectors(
        gs::integers<int>({
            .min_value = 0,
            .max_value = source_count * mappings_per_source - 1,
        }),
        {
            .min_size = 0,
            .max_size = source_count * mappings_per_source,
            .unique = true,
        }
    ));

    std::vector<MidiControlChangeMapping> mappings;
    mappings.reserve(mapping_keys.size());
    for (const int key : mapping_keys) {
        const int source = key / mappings_per_source;
        const int source_key = key % mappings_per_source;
        mappings.push_back({
            .source_port = source == 0 ? "Selected Controller" : "Other Controller",
            .channel = source_key / controller_count,
            .controller = source_key % controller_count,
            .target_controller = tc.draw(gs::integers<int>({
                .min_value = 0,
                .max_value = controller_count - 1,
            })),
        });
    }

    const int channel = tc.draw(gs::integers<int>({
        .min_value = 0,
        .max_value = channel_count - 1,
    }));
    const int controller = tc.draw(gs::integers<int>({
        .min_value = 0,
        .max_value = controller_count - 1,
    }));
    const int value = tc.draw(gs::integers<int>({
        .min_value = 0,
        .max_value = controller_count - 1,
    }));

    int expected_controller = controller;
    for (const auto& mapping : mappings) {
        if (mapping.source_port == "Selected Controller"
            && mapping.channel == channel
            && mapping.controller == controller) {
            expected_controller = mapping.target_controller;
            break;
        }
    }

    const MidiControlChangeMapper mapper{"Selected Controller", mappings};
    const auto mapped = mapper.map(controlChange(channel, controller, value));

    if (mapped.type != MidiMessageType::ControlChange
        || mapped.message.raw_type != raw(MidiMessageType::ControlChange)
        || mapped.message.channel != channel
        || mapped.message.control != expected_controller
        || mapped.message.value != value) {
        throw std::runtime_error("Control Change mapping disagrees with model");
    }
}

TEST(MidiControlChangeMapperPropertyTest, MatchesLinearModel) {
    control_change_mapping_matches_linear_model();
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
