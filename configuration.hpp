#pragma once

#include "midi_event.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace zeta {

enum class MidiControlType {
    Note,
    ControlChange,
    ProgramChange,
    MachineControl,
};

struct MidiControlBinding {
    MidiControlType type{};
    int channel{};
    int number{};
    int value{};
    bool match_any_program{};

    bool matches(MidiMessageType message_type, const MidiMessage& message) const noexcept;
    bool overlaps(const MidiControlBinding& other) const noexcept;
};

struct SoundFontDefinition {
    std::string id;
    std::filesystem::path file;
    int bank{};
    int preset{};
};

struct ApplicationConfig {
    std::vector<SoundFontDefinition> soundfonts;
    MidiControlBinding recording_control;
    MidiControlBinding next_soundfont_control;
};

class ConfigurationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

ApplicationConfig loadConfiguration(const std::filesystem::path& path);

} // namespace zeta
