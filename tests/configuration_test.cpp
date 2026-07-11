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

TEST(ConfigurationTest, ParsesOrderedCatalogPartsAndRecordingControls) {
    TemporaryConfig source{R"yaml(
schema_version: 1
soundfonts:
  - id: piano
    file: sounds/piano.sf2
    bank: 0
    preset: 4
  - id: bass
    file: /opt/sounds/bass.sf2
    bank: 128
    preset: 34
parts:
  live: piano
  loop: bass
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
    EXPECT_EQ(config.live_soundfont, "piano");
    EXPECT_EQ(config.loop_soundfont, "bass");

    ASSERT_EQ(config.recording_controls.size(), 3U);
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
}

TEST(ConfigurationTest, RejectsUnknownFields) {
    TemporaryConfig source{R"yaml(
schema_version: 1
soundfonts:
  - id: piano
    file: piano.sf2
    bank: 0
    preset: 0
    typo: true
parts:
  live: piano
  loop: piano
controls:
  recording:
    - type: program_change
      channel: 1
      program: 1
)yaml"};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsDuplicateSoundFontIds) {
    TemporaryConfig source{R"yaml(
schema_version: 1
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
  - { id: piano, file: other.sf2, bank: 0, preset: 1 }
parts: { live: piano, loop: piano }
controls:
  recording:
    - { type: note, channel: 1, key: 24 }
)yaml"};

    EXPECT_THROW(zeta::loadConfiguration(source.path()), ConfigurationError);
}

TEST(ConfigurationTest, RejectsUnknownPartAndOutOfRangeMidiValues) {
    TemporaryConfig unknown_part{R"yaml(
schema_version: 1
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
parts: { live: piano, loop: missing }
controls:
  recording:
    - { type: note, channel: 1, key: 24 }
)yaml"};
    EXPECT_THROW(zeta::loadConfiguration(unknown_part.path()), ConfigurationError);

    TemporaryConfig bad_channel{R"yaml(
schema_version: 1
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
parts: { live: piano, loop: piano }
controls:
  recording:
    - { type: program_change, channel: 0, program: 1 }
)yaml"};
    EXPECT_THROW(zeta::loadConfiguration(bad_channel.path()), ConfigurationError);
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
}

} // namespace
