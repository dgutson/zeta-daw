#include "../configuration.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {

using zeta::ConfigurationError;
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

TEST(ConfigurationTest, ParsesOrderedCatalogAndActionControls) {
    TemporaryConfig source{R"yaml(
schema_version: 2
soundfonts:
  - id: piano
    file: sounds/piano.sf2
    bank: 0
    preset: 4
  - id: bass
    file: /opt/sounds/bass.sf2
    bank: 128
    preset: 34
controls:
  recording:
    - type: note
      channel: 1
      key: 24
    - type: control_change
      channel: 2
      controller: 64
      value: 127
    - type: program_change
      channel: 16
      program: any
    - type: machine_control
      command: rewind
  next_soundfont:
    - type: machine_control
      command: stop
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

    ASSERT_EQ(config.recording_controls.size(), 4U);
    EXPECT_EQ(config.recording_controls[0].type, MidiControlType::Note);
    EXPECT_EQ(config.recording_controls[0].channel, 0);
    EXPECT_EQ(config.recording_controls[0].number, 24);
    EXPECT_EQ(config.recording_controls[1].type, MidiControlType::ControlChange);
    EXPECT_EQ(config.recording_controls[1].channel, 1);
    EXPECT_EQ(config.recording_controls[1].number, 64);
    EXPECT_EQ(config.recording_controls[1].value, 127);
    EXPECT_EQ(config.recording_controls[2].type, MidiControlType::ProgramChange);
    EXPECT_EQ(config.recording_controls[2].channel, 15);
    EXPECT_TRUE(config.recording_controls[2].match_any_program);
    EXPECT_EQ(config.recording_controls[3].type, MidiControlType::MachineControl);
    EXPECT_EQ(config.recording_controls[3].number, 0x05);

    ASSERT_EQ(config.next_soundfont_controls.size(), 1U);
    EXPECT_EQ(
        config.next_soundfont_controls[0].type,
        MidiControlType::MachineControl
    );
    EXPECT_EQ(config.next_soundfont_controls[0].number, 0x01);
}

TEST(ConfigurationTest, RejectsUnknownFields) {
    TemporaryConfig source{R"yaml(
schema_version: 2
soundfonts:
  - id: piano
    file: piano.sf2
    bank: 0
    preset: 0
    typo: true
controls:
  recording:
    - type: program_change
      channel: 1
      program: 1
  next_soundfont:
    - { type: machine_control, command: stop }
)yaml"};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsDuplicateSoundFontIds) {
    TemporaryConfig source{R"yaml(
schema_version: 2
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
  - { id: piano, file: other.sf2, bank: 0, preset: 1 }
controls:
  recording:
    - { type: note, channel: 1, key: 24 }
  next_soundfont:
    - { type: machine_control, command: stop }
)yaml"};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsOldSchemaAndRemovedParts) {
    TemporaryConfig old_schema{R"yaml(
schema_version: 1
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
controls:
  recording:
    - { type: note, channel: 1, key: 24 }
  next_soundfont:
    - { type: machine_control, command: stop }
)yaml"};
    EXPECT_THROW(zeta::loadConfiguration(old_schema.path()), ConfigurationError);

    TemporaryConfig removed_parts{R"yaml(
schema_version: 2
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
parts: { live: piano, loop: piano }
controls:
  recording:
    - { type: note, channel: 1, key: 24 }
  next_soundfont:
    - { type: machine_control, command: stop }
)yaml"};
    EXPECT_THROW(zeta::loadConfiguration(removed_parts.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsInvalidMidiBindings) {
    TemporaryConfig bad_channel{R"yaml(
schema_version: 2
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
controls:
  recording:
    - { type: program_change, channel: 0, program: 1 }
  next_soundfont:
    - { type: machine_control, command: stop }
)yaml"};
    EXPECT_THROW(zeta::loadConfiguration(bad_channel.path()), ConfigurationError);

    TemporaryConfig bad_mmc_command{R"yaml(
schema_version: 2
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
controls:
  recording:
    - { type: machine_control, command: rewind }
  next_soundfont:
    - { type: machine_control, command: dance }
)yaml"};
    EXPECT_THROW(
        zeta::loadConfiguration(bad_mmc_command.path()),
        ConfigurationError
    );
}

TEST(ConfigurationTest, RejectsOverlappingActionBindings) {
    TemporaryConfig source{R"yaml(
schema_version: 2
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
controls:
  recording:
    - { type: program_change, channel: 1, program: any }
  next_soundfont:
    - { type: program_change, channel: 1, program: 12 }
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
