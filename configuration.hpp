#pragma once

#include "midi_control_change_mapping.hpp"
#include "midi_event.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
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
    std::optional<int> key;
};

struct LoopSlotDefinition {
    int key{};
};

struct AudioConfig {
    static constexpr double default_gain = 0.5;

    std::optional<std::string> driver;
    std::optional<std::string> alsa_device;
    double gain{default_gain};
};

struct ApplicationConfig {
    AudioConfig audio;
    std::vector<LoopSlotDefinition> loop_slots;
    std::vector<SoundFontDefinition> soundfonts;
    std::vector<MidiControlChangeMapping> midi_control_change_mappings;
    MidiControlBinding loop_slot_by_note_control;
    std::optional<MidiControlBinding> next_soundfont_control;
    std::optional<MidiControlBinding> soundfont_by_note_control;
    MidiControlBinding octave_down_control;
    MidiControlBinding octave_up_control;
};

class ConfigurationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

ApplicationConfig loadConfiguration(const std::filesystem::path& path);

} // namespace zeta
