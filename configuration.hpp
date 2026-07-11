#pragma once

#include "looper_fsm.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zeta {

enum class MidiControlType {
    Note,
    ControlChange,
    ProgramChange,
};

struct MidiControlBinding {
    MidiControlType type{};
    int channel{};
    int number{};
    int value{};
    bool match_any_program{};

    bool matches(MidiMessageType message_type, const MidiMessage& message) const noexcept;
};

struct SoundFontDefinition {
    std::string id;
    std::filesystem::path file;
    int bank{};
    int preset{};
};

struct ApplicationConfig {
    std::vector<SoundFontDefinition> soundfonts;
    std::string live_soundfont;
    std::string loop_soundfont;
    std::vector<MidiControlBinding> recording_controls;

    const SoundFontDefinition& soundfont(std::string_view id) const;
};

class ConfigurationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

ApplicationConfig loadConfiguration(const std::filesystem::path& path);

} // namespace zeta
