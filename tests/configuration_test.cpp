#include "../configuration.hpp"

#include <gtest/gtest.h>
#include <hegel/hegel.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace gs = hegel::generators;

using zeta::ConfigurationError;
using zeta::MidiControlBinding;
using zeta::MidiMessage;
using zeta::MidiMessageType;
using zeta::MidiControlType;

class TemporaryConfig final {
public:
    explicit TemporaryConfig(std::string contents)
        : path_(std::filesystem::temp_directory_path()
            / ("zeta-daw-config-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            ) + ".yaml")) {
        std::ofstream output{path_};
        output << contents;
    }

    ~TemporaryConfig() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryConfig(const TemporaryConfig&) = delete;
    TemporaryConfig& operator=(const TemporaryConfig&) = delete;

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string configWithMmcCommands(
    const std::array<std::string_view, 4>& commands
) {
    std::ostringstream config;
    config
        << "schema_version: 6\n"
        << "midi_control_change_mappings: []\n"
        << "soundfonts:\n"
        << "  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }\n"
        << "controls:\n"
        << "  recording: { type: machine_control, command: "
        << commands[0] << " }\n"
        << "  next_soundfont: { type: machine_control, command: "
        << commands[1] << " }\n"
        << "  octave_down: { type: machine_control, command: "
        << commands[2] << " }\n"
        << "  octave_up: { type: machine_control, command: "
        << commands[3] << " }\n";
    return config.str();
}

std::string configWithMapping(std::string_view mapping) {
    std::ostringstream config;
    config
        << "schema_version: 6\n"
        << "midi_control_change_mappings:\n"
        << "  - { " << mapping << " }\n"
        << "soundfonts:\n"
        << "  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }\n"
        << "controls:\n"
        << "  recording: { type: machine_control, command: rewind }\n"
        << "  next_soundfont: { type: machine_control, command: stop }\n"
        << "  octave_down: { type: machine_control, command: play }\n"
        << "  octave_up: { type: machine_control, command: record_strobe }\n";
    return config.str();
}

std::string configWithAllMmcCommands(
    const std::array<std::string_view, 5>& commands
) {
    std::ostringstream config;
    config
        << "schema_version: 6\n"
        << "soundfonts:\n"
        << "  - { id: piano, file: piano.sf2, bank: 0, preset: 0, key: C4 }\n"
        << "controls:\n"
        << "  recording: { type: machine_control, command: "
        << commands[0] << " }\n"
        << "  next_soundfont: { type: machine_control, command: "
        << commands[1] << " }\n"
        << "  soundfont_by_note: { type: machine_control, command: "
        << commands[2] << " }\n"
        << "  octave_down: { type: machine_control, command: "
        << commands[3] << " }\n"
        << "  octave_up: { type: machine_control, command: "
        << commands[4] << " }\n";
    return config.str();
}

MidiControlType generatedControlType(int choice) {
    switch (choice) {
    case 0:
        return MidiControlType::Note;
    case 1:
        return MidiControlType::ControlChange;
    case 2:
        return MidiControlType::ProgramChange;
    default:
        return MidiControlType::MachineControl;
    }
}

MidiControlBinding generatedBinding(hegel::TestCase& tc) {
    return {
        .type = generatedControlType(
            tc.draw(gs::integers<int>({.min_value = 0, .max_value = 3}))
        ),
        .channel = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 15})),
        .number = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 127})),
        .value = tc.draw(gs::integers<int>({.min_value = 0, .max_value = 127})),
        .match_any_program = tc.draw(gs::booleans()),
    };
}

bool bothMatch(
    const MidiControlBinding& first,
    const MidiControlBinding& second,
    MidiMessageType type,
    const MidiMessage& message
) {
    return first.matches(type, message) && second.matches(type, message);
}

bool haveCommonMatchingEvent(
    const MidiControlBinding& first,
    const MidiControlBinding& second
) {
    for (int channel = 0; channel < 16; ++channel) {
        for (int number = 0; number < 128; ++number) {
            if (bothMatch(
                    first,
                    second,
                    MidiMessageType::NoteOn,
                    MidiMessage{
                        .channel = channel,
                        .key = number,
                        .velocity = 1,
                    }
                )) {
                return true;
            }

            if (bothMatch(
                    first,
                    second,
                    MidiMessageType::ProgramChange,
                    MidiMessage{.channel = channel, .program = number}
                )) {
                return true;
            }

            for (int value = 0; value < 128; ++value) {
                if (bothMatch(
                        first,
                        second,
                        MidiMessageType::ControlChange,
                        MidiMessage{
                            .channel = channel,
                            .control = number,
                            .value = value,
                        }
                    )) {
                    return true;
                }
            }
        }
    }

    for (int command = 0; command < 128; ++command) {
        if (bothMatch(
                first,
                second,
                MidiMessageType::MachineControl,
                MidiMessage{.machine_control_command = command}
            )) {
            return true;
        }
    }
    return false;
}

HEGEL_TEST(control_binding_overlap_is_symmetric)(hegel::TestCase& tc) {
    const auto first = generatedBinding(tc);
    const auto second = generatedBinding(tc);

    if (first.overlaps(second) != second.overlaps(first)) {
        throw std::runtime_error("MIDI control-binding overlap is asymmetric");
    }
}

HEGEL_TEST(control_binding_overlap_matches_finite_event_model)(
    hegel::TestCase& tc
) {
    const auto first = generatedBinding(tc);
    const auto second = generatedBinding(tc);
    const bool expected = haveCommonMatchingEvent(first, second);

    if (first.overlaps(second) != expected) {
        throw std::runtime_error(
            "MIDI control-binding overlap disagrees with matching events"
        );
    }
}

TEST(MidiControlBindingPropertyTest, OverlapIsSymmetric) {
    control_binding_overlap_is_symmetric();
}

TEST(MidiControlBindingPropertyTest, OverlapMatchesFiniteEventModel) {
    control_binding_overlap_matches_finite_event_model();
}

TEST(ConfigurationTest, ParsesCatalogMappingsAndActionControls) {
    TemporaryConfig source{R"yaml(
schema_version: 6
midi_control_change_mappings:
  - source_port: Controller MIDI2
    channel: 16
    controller: 20
    target_controller: 7
soundfonts:
  - id: piano
    file: sounds/piano.sf2
    bank: 0
    preset: 4
    key: G3
  - id: bass
    file: /opt/sounds/bass.sf2
    bank: 128
    preset: 34
    key: C#4
controls:
  recording:
    type: note
    channel: 1
    key: 24
  next_soundfont:
    type: machine_control
    command: stop
  soundfont_by_note: { type: machine_control, command: pause }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};

    const auto config = zeta::loadConfiguration(source.path());

    ASSERT_EQ(config.soundfonts.size(), 2U);
    EXPECT_EQ(config.soundfonts[0].id, "piano");
    EXPECT_EQ(
        config.soundfonts[0].file,
        (source.path().parent_path() / "sounds/piano.sf2").lexically_normal()
    );
    EXPECT_EQ(config.soundfonts[1].id, "bass");
    EXPECT_EQ(config.soundfonts[1].bank, 128);

    ASSERT_EQ(config.midi_control_change_mappings.size(), 1U);
    EXPECT_EQ(
        config.midi_control_change_mappings[0].source_port,
        "Controller MIDI2"
    );
    EXPECT_EQ(config.midi_control_change_mappings[0].channel, 15);
    EXPECT_EQ(config.midi_control_change_mappings[0].controller, 20);
    EXPECT_EQ(config.midi_control_change_mappings[0].target_controller, 7);

    EXPECT_EQ(config.recording_control.type, MidiControlType::Note);
    EXPECT_EQ(config.recording_control.channel, 0);
    EXPECT_EQ(config.recording_control.number, 24);
    ASSERT_TRUE(config.next_soundfont_control);
    EXPECT_EQ(
        config.next_soundfont_control->type,
        MidiControlType::MachineControl
    );
    EXPECT_EQ(config.next_soundfont_control->number, 0x01);
    ASSERT_TRUE(config.soundfont_by_note_control);
    EXPECT_EQ(config.soundfont_by_note_control->number, 0x09);
    EXPECT_EQ(config.octave_down_control.number, 0x02);
    EXPECT_EQ(config.octave_up_control.number, 0x06);

    ASSERT_TRUE(config.soundfonts[0].key);
    EXPECT_EQ(*config.soundfonts[0].key, 67);
    ASSERT_TRUE(config.soundfonts[1].key);
    EXPECT_EQ(*config.soundfonts[1].key, 73);
}

TEST(ConfigurationTest, AcceptsEitherOrBothSoundFontSelectionControls) {
    TemporaryConfig only_next{configWithMmcCommands({
        "rewind",
        "stop",
        "play",
        "record_strobe",
    })};
    const auto next_config = zeta::loadConfiguration(only_next.path());
    EXPECT_TRUE(next_config.next_soundfont_control);
    EXPECT_FALSE(next_config.soundfont_by_note_control);

    auto only_direct_contents = configWithAllMmcCommands({
        "rewind",
        "stop",
        "pause",
        "play",
        "record_strobe",
    });
    const std::string next =
        "  next_soundfont: { type: machine_control, command: stop }\n";
    only_direct_contents.erase(only_direct_contents.find(next), next.size());
    TemporaryConfig only_direct{std::move(only_direct_contents)};
    const auto direct_config = zeta::loadConfiguration(only_direct.path());
    EXPECT_FALSE(direct_config.next_soundfont_control);
    EXPECT_TRUE(direct_config.soundfont_by_note_control);

    TemporaryConfig both{configWithAllMmcCommands({
        "rewind",
        "stop",
        "pause",
        "play",
        "record_strobe",
    })};
    const auto both_config = zeta::loadConfiguration(both.path());
    EXPECT_TRUE(both_config.next_soundfont_control);
    EXPECT_TRUE(both_config.soundfont_by_note_control);
}

TEST(ConfigurationTest, AcceptsOmittedMidiControlChangeMappings) {
    auto contents = configWithMmcCommands({
        "rewind",
        "stop",
        "play",
        "record_strobe",
    });
    const std::string field = "midi_control_change_mappings: []\n";
    contents.erase(contents.find(field), field.size());
    TemporaryConfig source{std::move(contents)};

    const auto config = zeta::loadConfiguration(source.path());

    EXPECT_TRUE(config.midi_control_change_mappings.empty());
}

TEST(ConfigurationTest, AcceptsEmptyMidiControlChangeMappings) {
    TemporaryConfig source{configWithMmcCommands({
        "rewind",
        "stop",
        "play",
        "record_strobe",
    })};

    const auto config = zeta::loadConfiguration(source.path());

    EXPECT_TRUE(config.midi_control_change_mappings.empty());
}

TEST(ConfigurationTest, RejectsInvalidMidiControlChangeMappings) {
    constexpr std::array invalid_mappings{
        std::string_view{
            "source_port: '', channel: 16, controller: 20, "
            "target_controller: 7"
        },
        std::string_view{
            "source_port: MIDI2, channel: 0, controller: 20, "
            "target_controller: 7"
        },
        std::string_view{
            "source_port: MIDI2, channel: 16, controller: 128, "
            "target_controller: 7"
        },
        std::string_view{
            "source_port: MIDI2, channel: 16, controller: 20, "
            "target_controller: 128"
        },
        std::string_view{
            "source_port: MIDI2, channel: 16, controller: 20"
        },
        std::string_view{
            "source_port: MIDI2, channel: 16, controller: 20, "
            "target_controller: 7, typo: true"
        },
    };

    for (const auto mapping : invalid_mappings) {
        TemporaryConfig source{configWithMapping(mapping)};
        SCOPED_TRACE(mapping);
        EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
    }
}

TEST(ConfigurationTest, RejectsDuplicateMidiControlChangeMappings) {
    auto contents = configWithMapping(
        "source_port: MIDI2, channel: 16, controller: 20, "
        "target_controller: 7"
    );
    const std::string first_mapping =
        "  - { source_port: MIDI2, channel: 16, controller: 20, "
        "target_controller: 7 }\n";
    const auto position = contents.find(first_mapping);
    contents.insert(
        position + first_mapping.size(),
        "  - { source_port: MIDI2, channel: 16, controller: 20, "
        "target_controller: 74 }\n"
    );
    TemporaryConfig source{std::move(contents)};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsEveryMissingActionControl) {
    constexpr std::array action_names{
        std::string_view{"recording"},
        std::string_view{"octave_down"},
        std::string_view{"octave_up"},
    };
    constexpr std::array commands{
        std::string_view{"rewind"},
        std::string_view{"stop"},
        std::string_view{"play"},
        std::string_view{"record_strobe"},
    };

    for (const auto action : action_names) {
        auto contents = configWithMmcCommands(commands);
        const std::string prefix = "  " + std::string{action} + ":";
        const auto start = contents.find(prefix);
        const auto finish = contents.find('\n', start);
        contents.erase(start, finish - start + 1);

        TemporaryConfig source{std::move(contents)};
        SCOPED_TRACE(action);
        EXPECT_THROW(
            zeta::loadConfiguration(source.path()),
            ConfigurationError
        );
    }
}

TEST(ConfigurationTest, RejectsMissingSoundFontSelectionMechanism) {
    auto contents = configWithMmcCommands({
        "rewind",
        "stop",
        "play",
        "record_strobe",
    });
    const std::string next =
        "  next_soundfont: { type: machine_control, command: stop }\n";
    contents.erase(contents.find(next), next.size());
    TemporaryConfig source{std::move(contents)};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsInvalidSoundFontKeys) {
    constexpr std::array invalid_keys{
        std::string_view{""},
        std::string_view{"60"},
        std::string_view{"Cb4"},
        std::string_view{"G#9"},
        std::string_view{"C0"},
        std::string_view{"C-1"},
        std::string_view{"C-2"},
        std::string_view{"C#5"},
        std::string_view{"c4"},
    };

    for (const auto key : invalid_keys) {
        std::ostringstream contents;
        contents
            << "schema_version: 6\n"
            << "soundfonts:\n"
            << "  - { id: piano, file: piano.sf2, bank: 0, preset: 0, key: '"
            << key << "' }\n"
            << "controls:\n"
            << "  recording: { type: machine_control, command: rewind }\n"
            << "  soundfont_by_note: { type: machine_control, command: stop }\n"
            << "  octave_down: { type: machine_control, command: play }\n"
            << "  octave_up: { type: machine_control, command: record_strobe }\n";
        TemporaryConfig source{contents.str()};
        SCOPED_TRACE(key);
        EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
    }
}

TEST(ConfigurationTest, ParsesSe49PhysicalKeybedBoundaries) {
    TemporaryConfig source{R"yaml(
schema_version: 6
soundfonts:
  - { id: lowest, file: lowest.sf2, bank: 0, preset: 0, key: C1 }
  - { id: highest, file: highest.sf2, bank: 0, preset: 0, key: C5 }
controls:
  recording: { type: machine_control, command: rewind }
  soundfont_by_note: { type: machine_control, command: stop }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};

    const auto config = zeta::loadConfiguration(source.path());

    ASSERT_TRUE(config.soundfonts[0].key);
    EXPECT_EQ(config.soundfonts[0].key.value(), 36);
    ASSERT_TRUE(config.soundfonts[1].key);
    EXPECT_EQ(config.soundfonts[1].key.value(), 84);
}

TEST(ConfigurationTest, RejectsDuplicateSoundFontKeys) {
    TemporaryConfig source{R"yaml(
schema_version: 6
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0, key: G3 }
  - { id: bass, file: bass.sf2, bank: 0, preset: 34, key: G3 }
controls:
  recording: { type: machine_control, command: rewind }
  soundfont_by_note: { type: machine_control, command: stop }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsSelectionNotesOverlappingActionBindings) {
    TemporaryConfig source{R"yaml(
schema_version: 6
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0, key: C4 }
controls:
  recording: { type: note, channel: 16, key: 72 }
  soundfont_by_note: { type: machine_control, command: stop }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, RequiresKeysExactlyWhenDirectSelectionIsConfigured) {
    auto missing_contents = configWithAllMmcCommands({
        "rewind",
        "stop",
        "pause",
        "play",
        "record_strobe",
    });
    const std::string key = ", key: C4";
    missing_contents.erase(missing_contents.find(key), key.size());
    TemporaryConfig missing{std::move(missing_contents)};
    EXPECT_THROW(zeta::loadConfiguration(missing.path()), ConfigurationError);

    auto unused_contents = configWithAllMmcCommands({
        "rewind",
        "stop",
        "pause",
        "play",
        "record_strobe",
    });
    const std::string direct =
        "  soundfont_by_note: { type: machine_control, command: pause }\n";
    unused_contents.erase(unused_contents.find(direct), direct.size());
    TemporaryConfig unused{std::move(unused_contents)};
    EXPECT_THROW(zeta::loadConfiguration(unused.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsUnknownFields) {
    TemporaryConfig source{R"yaml(
schema_version: 6
midi_control_change_mappings: []
soundfonts:
  - id: piano
    file: piano.sf2
    bank: 0
    preset: 0
    typo: true
controls:
  recording:
    type: program_change
    channel: 1
    program: 1
  next_soundfont: { type: machine_control, command: stop }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsDuplicateSoundFontIds) {
    TemporaryConfig source{R"yaml(
schema_version: 6
midi_control_change_mappings: []
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
  - { id: piano, file: other.sf2, bank: 0, preset: 1 }
controls:
  recording: { type: note, channel: 1, key: 24 }
  next_soundfont: { type: machine_control, command: stop }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsOldSchemaAndRemovedParts) {
    TemporaryConfig old_schema{R"yaml(
schema_version: 5
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
controls:
  recording: { type: note, channel: 1, key: 24 }
  next_soundfont: { type: machine_control, command: stop }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};
    try {
        zeta::loadConfiguration(old_schema.path());
        FAIL() << "Expected the old schema version to be rejected";
    } catch (const ConfigurationError& error) {
        EXPECT_STREQ(
            error.what(),
            "configuration.schema_version: unsupported schema version 5; "
            "expected 6"
        );
    }

    TemporaryConfig removed_parts{R"yaml(
schema_version: 6
midi_control_change_mappings: []
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
parts: { live: piano, loop: piano }
controls:
  recording: { type: note, channel: 1, key: 24 }
  next_soundfont: { type: machine_control, command: stop }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};
    EXPECT_THROW(zeta::loadConfiguration(removed_parts.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsInvalidMidiBindings) {
    TemporaryConfig bad_channel{R"yaml(
schema_version: 6
midi_control_change_mappings: []
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
controls:
  recording: { type: program_change, channel: 0, program: 1 }
  next_soundfont: { type: machine_control, command: stop }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};
    EXPECT_THROW(zeta::loadConfiguration(bad_channel.path()), ConfigurationError);

    TemporaryConfig bad_mmc_command{R"yaml(
schema_version: 6
midi_control_change_mappings: []
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
controls:
  recording: { type: machine_control, command: rewind }
  next_soundfont: { type: machine_control, command: dance }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};
    EXPECT_THROW(
        zeta::loadConfiguration(bad_mmc_command.path()),
        ConfigurationError
    );
}

TEST(ConfigurationTest, RejectsOverlappingActionBindings) {
    constexpr std::array action_names{
        std::string_view{"recording"},
        std::string_view{"next_soundfont"},
        std::string_view{"soundfont_by_note"},
        std::string_view{"octave_down"},
        std::string_view{"octave_up"},
    };

    for (std::size_t first = 0; first < action_names.size(); ++first) {
        for (
            std::size_t second = first + 1;
            second < action_names.size();
            ++second
        ) {
            auto commands = std::array<std::string_view, 5>{
                "rewind",
                "stop",
                "pause",
                "play",
                "record_strobe",
            };
            commands[first] = "pause";
            commands[second] = "pause";
            TemporaryConfig source{configWithAllMmcCommands(commands)};
            SCOPED_TRACE(
                std::string{action_names[first]} + " and "
                    + std::string{action_names[second]}
            );
            EXPECT_THROW(
                zeta::loadConfiguration(source.path()),
                ConfigurationError
            );
        }
    }
}

TEST(ConfigurationTest, RejectsMultipleBindingsForAnAction) {
    TemporaryConfig source{R"yaml(
schema_version: 6
midi_control_change_mappings: []
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
controls:
  recording:
    - { type: machine_control, command: rewind }
    - { type: control_change, channel: 1, controller: 20, value: 127 }
  next_soundfont: { type: machine_control, command: stop }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)yaml"};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, MatchesSupportedMidiControlsPrecisely) {
    const zeta::MidiControlBinding note{
        .type = MidiControlType::Note,
        .channel = 0,
        .number = 24,
    };
    EXPECT_TRUE(note.matches(
        MidiMessageType::NoteOn,
        MidiMessage{.channel = 0, .key = 24, .velocity = 100}
    ));
    EXPECT_FALSE(note.matches(
        MidiMessageType::NoteOn,
        MidiMessage{.channel = 0, .key = 24, .velocity = 0}
    ));

    const zeta::MidiControlBinding control_change{
        .type = MidiControlType::ControlChange,
        .channel = 1,
        .number = 64,
        .value = 127,
    };
    EXPECT_TRUE(control_change.matches(
        MidiMessageType::ControlChange,
        MidiMessage{.channel = 1, .control = 64, .value = 127}
    ));
    EXPECT_FALSE(control_change.matches(
        MidiMessageType::ControlChange,
        MidiMessage{.channel = 1, .control = 64, .value = 0}
    ));

    const zeta::MidiControlBinding program_change{
        .type = MidiControlType::ProgramChange,
        .channel = 15,
        .number = 9,
    };
    EXPECT_TRUE(program_change.matches(
        MidiMessageType::ProgramChange,
        MidiMessage{.channel = 15, .program = 9}
    ));
    EXPECT_FALSE(program_change.matches(
        MidiMessageType::ProgramChange,
        MidiMessage{.channel = 14, .program = 9}
    ));

    const zeta::MidiControlBinding any_program_change{
        .type = MidiControlType::ProgramChange,
        .channel = 0,
        .match_any_program = true,
    };
    EXPECT_TRUE(any_program_change.matches(
        MidiMessageType::ProgramChange,
        MidiMessage{.channel = 0, .program = 0}
    ));
    EXPECT_TRUE(any_program_change.matches(
        MidiMessageType::ProgramChange,
        MidiMessage{.channel = 0, .program = 127}
    ));
    EXPECT_FALSE(any_program_change.matches(
        MidiMessageType::ProgramChange,
        MidiMessage{.channel = 1, .program = 42}
    ));

    const zeta::MidiControlBinding machine_control{
        .type = MidiControlType::MachineControl,
        .number = 0x05,
    };
    EXPECT_TRUE(machine_control.matches(
        MidiMessageType::MachineControl,
        MidiMessage{.machine_control_command = 0x05}
    ));
    EXPECT_FALSE(machine_control.matches(
        MidiMessageType::MachineControl,
        MidiMessage{.machine_control_command = 0x01}
    ));

    EXPECT_TRUE(any_program_change.overlaps(zeta::MidiControlBinding{
        .type = MidiControlType::ProgramChange,
        .channel = 0,
        .number = 12,
    }));
    EXPECT_FALSE(any_program_change.overlaps(machine_control));
}

} // namespace
