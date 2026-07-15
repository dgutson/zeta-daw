#include "configuration.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace zeta {
namespace {

constexpr int required_schema_version = 6;

struct NamedMachineControlCommand {
    std::string_view name;
    int value;
};

constexpr std::array machine_control_commands{
    NamedMachineControlCommand{"stop", 0x01},
    NamedMachineControlCommand{"play", 0x02},
    NamedMachineControlCommand{"deferred_play", 0x03},
    NamedMachineControlCommand{"fast_forward", 0x04},
    NamedMachineControlCommand{"rewind", 0x05},
    NamedMachineControlCommand{"record_strobe", 0x06},
    NamedMachineControlCommand{"record_exit", 0x07},
    NamedMachineControlCommand{"record_pause", 0x08},
    NamedMachineControlCommand{"pause", 0x09},
    NamedMachineControlCommand{"eject", 0x0A},
    NamedMachineControlCommand{"chase", 0x0B},
    NamedMachineControlCommand{"reset", 0x0D},
};

[[noreturn]] void fail(const std::string& location, const std::string& message) {
    throw ConfigurationError(location + ": " + message);
}

void requireMap(const YAML::Node& node, const std::string& location) {
    if (!node || !node.IsMap()) {
        fail(location, "expected a mapping");
    }
}

void requireSequence(const YAML::Node& node, const std::string& location) {
    if (!node || !node.IsSequence()) {
        fail(location, "expected a sequence");
    }
}

void rejectUnknownKeys(
    const YAML::Node& node,
    const std::string& location,
    std::initializer_list<std::string_view> allowed
) {
    requireMap(node, location);
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) {
            fail(location, "mapping keys must be strings");
        }

        const auto key = entry.first.as<std::string>();
        if (std::ranges::find(allowed, key) == allowed.end()) {
            fail(location, "unknown field '" + key + "'");
        }
    }
}

template <typename T>
T required(const YAML::Node& node, const char* key, const std::string& location) {
    const auto value = node[key];
    if (!value) {
        fail(location, "missing required field '" + std::string{key} + "'");
    }

    try {
        return value.as<T>();
    } catch (const YAML::Exception& error) {
        fail(location + "." + key, error.what());
    }
}

int boundedInt(
    const YAML::Node& node,
    const char* key,
    const std::string& location,
    int minimum,
    int maximum
) {
    const int value = required<int>(node, key, location);
    if (value < minimum || value > maximum) {
        fail(
            location + "." + key,
            "expected a value from " + std::to_string(minimum) + " to "
                + std::to_string(maximum)
        );
    }
    return value;
}

std::string nonEmptyString(
    const YAML::Node& node,
    const char* key,
    const std::string& location
) {
    auto value = required<std::string>(node, key, location);
    if (value.empty()) {
        fail(location + "." + key, "must not be empty");
    }
    return value;
}

int parseMachineControlCommand(
    const YAML::Node& node,
    const std::string& location
) {
    const auto command = nonEmptyString(node, "command", location);
    const auto found = std::ranges::find(
        machine_control_commands,
        command,
        &NamedMachineControlCommand::name
    );
    if (found == machine_control_commands.end()) {
        fail(location + ".command", "unsupported MMC command '" + command + "'");
    }
    return found->value;
}

MidiControlBinding parseControl(
    const YAML::Node& node,
    const std::string& location
) {
    requireMap(node, location);

    const auto type = nonEmptyString(node, "type", location);

    if (type == "machine_control") {
        rejectUnknownKeys(node, location, {"type", "command"});
        return {
            .type = MidiControlType::MachineControl,
            .number = parseMachineControlCommand(node, location),
        };
    }

    if (type != "note"
        && type != "control_change"
        && type != "program_change") {
        fail(location + ".type", "unsupported MIDI control type '" + type + "'");
    }

    const int channel = boundedInt(node, "channel", location, 1, 16) - 1;

    if (type == "note") {
        rejectUnknownKeys(node, location, {"type", "channel", "key"});
        return {
            .type = MidiControlType::Note,
            .channel = channel,
            .number = boundedInt(node, "key", location, 0, 127),
        };
    }

    if (type == "control_change") {
        rejectUnknownKeys(
            node,
            location,
            {"type", "channel", "controller", "value"}
        );
        return {
            .type = MidiControlType::ControlChange,
            .channel = channel,
            .number = boundedInt(node, "controller", location, 0, 127),
            .value = boundedInt(node, "value", location, 0, 127),
        };
    }

    rejectUnknownKeys(node, location, {"type", "channel", "program"});
    const auto program = node["program"];
    if (!program) {
        fail(location, "missing required field 'program'");
    }
    if (program.IsScalar() && program.Scalar() == "any") {
        return {
            .type = MidiControlType::ProgramChange,
            .channel = channel,
            .match_any_program = true,
        };
    }
    return {
        .type = MidiControlType::ProgramChange,
        .channel = channel,
        .number = boundedInt(node, "program", location, 0, 127),
    };
}

MidiControlBinding parseActionControl(
    const YAML::Node& controls,
    const char* name
) {
    const std::string location = "controls." + std::string{name};
    return parseControl(controls[name], location);
}

std::optional<MidiControlBinding> parseOptionalActionControl(
    const YAML::Node& controls,
    const char* name
) {
    if (!controls[name]) {
        return std::nullopt;
    }
    return parseActionControl(controls, name);
}

std::vector<MidiControlChangeMapping> parseMidiControlChangeMappings(
    const YAML::Node& mappings
) {
    constexpr std::string_view mappings_location{
        "midi_control_change_mappings"
    };
    requireSequence(mappings, std::string{mappings_location});

    std::vector<MidiControlChangeMapping> parsed;
    parsed.reserve(mappings.size());

    for (std::size_t index = 0; index < mappings.size(); ++index) {
        const auto node = mappings[index];
        const std::string location = std::string{mappings_location}
            + "[" + std::to_string(index) + "]";
        rejectUnknownKeys(node, location, {
            "source_port",
            "channel",
            "controller",
            "target_controller",
        });

        MidiControlChangeMapping mapping{
            .source_port = nonEmptyString(node, "source_port", location),
            .channel = boundedInt(node, "channel", location, 1, 16) - 1,
            .controller = boundedInt(node, "controller", location, 0, 127),
            .target_controller = boundedInt(
                node,
                "target_controller",
                location,
                0,
                127
            ),
        };

        const auto duplicate = std::ranges::find_if(
            parsed,
            [&](const MidiControlChangeMapping& candidate) {
                return candidate.source_port == mapping.source_port
                    && candidate.channel == mapping.channel
                    && candidate.controller == mapping.controller;
            }
        );
        if (duplicate != parsed.end()) {
            fail(
                location,
                "duplicate source_port, channel, and controller mapping"
            );
        }

        parsed.push_back(std::move(mapping));
    }

    return parsed;
}

std::filesystem::path resolveSoundFontPath(
    const std::filesystem::path& config_path,
    const std::string& configured_path
) {
    std::filesystem::path path{configured_path};
    if (path.is_relative()) {
        path = config_path.parent_path() / path;
    }
    return std::filesystem::absolute(path).lexically_normal();
}

} // namespace

bool MidiControlBinding::matches(
    MidiMessageType message_type,
    const MidiMessage& message
) const noexcept {
    switch (type) {
    case MidiControlType::Note:
        return message_type == MidiMessageType::NoteOn
            && message.channel == channel
            && message.velocity > 0
            && message.key == number;
    case MidiControlType::ControlChange:
        return message_type == MidiMessageType::ControlChange
            && message.channel == channel
            && message.control == number
            && message.value == value;
    case MidiControlType::ProgramChange:
        return message_type == MidiMessageType::ProgramChange
            && message.channel == channel
            && (match_any_program || message.program == number);
    case MidiControlType::MachineControl:
        return message_type == MidiMessageType::MachineControl
            && message.machine_control_command == number;
    }
    return false;
}

bool MidiControlBinding::overlaps(const MidiControlBinding& other) const noexcept {
    if (type != other.type) {
        return false;
    }

    switch (type) {
    case MidiControlType::Note:
        return channel == other.channel && number == other.number;
    case MidiControlType::ControlChange:
        return channel == other.channel
            && number == other.number
            && value == other.value;
    case MidiControlType::ProgramChange:
        return channel == other.channel
            && (match_any_program
                || other.match_any_program
                || number == other.number);
    case MidiControlType::MachineControl:
        return number == other.number;
    }
    return false;
}

ApplicationConfig loadConfiguration(const std::filesystem::path& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& error) {
        throw ConfigurationError("could not read configuration '" + path.string()
            + "': " + error.what());
    }

    rejectUnknownKeys(root, "configuration", {
        "schema_version",
        "midi_control_change_mappings",
        "soundfonts",
        "soundfont_note_selections",
        "controls",
    });

    const int schema_version = required<int>(
        root,
        "schema_version",
        "configuration"
    );
    if (schema_version != required_schema_version) {
        fail(
            "configuration.schema_version",
            "unsupported schema version " + std::to_string(schema_version)
                + "; expected " + std::to_string(required_schema_version)
        );
    }

    ApplicationConfig config;
    const auto midi_control_change_mappings =
        root["midi_control_change_mappings"];
    if (midi_control_change_mappings) {
        config.midi_control_change_mappings = parseMidiControlChangeMappings(
            midi_control_change_mappings
        );
    }

    const auto soundfonts = root["soundfonts"];
    requireSequence(soundfonts, "soundfonts");
    if (soundfonts.size() == 0) {
        fail("soundfonts", "must contain at least one entry");
    }

    std::unordered_map<std::string, std::size_t> soundfont_indices;
    for (std::size_t index = 0; index < soundfonts.size(); ++index) {
        const auto node = soundfonts[index];
        const std::string location = "soundfonts[" + std::to_string(index) + "]";
        rejectUnknownKeys(node, location, {"id", "file", "bank", "preset"});

        auto id = nonEmptyString(node, "id", location);
        if (!soundfont_indices.emplace(id, index).second) {
            fail(location + ".id", "duplicate SoundFont id '" + id + "'");
        }

        config.soundfonts.push_back({
            .id = std::move(id),
            .file = resolveSoundFontPath(
                std::filesystem::absolute(path),
                nonEmptyString(node, "file", location)
            ),
            .bank = boundedInt(node, "bank", location, 0, 16383),
            .preset = boundedInt(node, "preset", location, 0, 127),
        });
    }

    const auto controls = root["controls"];
    rejectUnknownKeys(controls, "controls", {
        "recording",
        "next_soundfont",
        "soundfont_by_note",
        "octave_down",
        "octave_up",
    });
    config.recording_control = parseActionControl(controls, "recording");
    config.next_soundfont_control = parseOptionalActionControl(
        controls,
        "next_soundfont"
    );
    config.soundfont_by_note_control = parseOptionalActionControl(
        controls,
        "soundfont_by_note"
    );
    config.octave_down_control = parseActionControl(controls, "octave_down");
    config.octave_up_control = parseActionControl(controls, "octave_up");

    if (!config.next_soundfont_control && !config.soundfont_by_note_control) {
        fail(
            "controls",
            "at least one of next_soundfont or soundfont_by_note is required"
        );
    }

    const auto soundfont_note_selections = root["soundfont_note_selections"];
    if (!config.soundfont_by_note_control) {
        if (soundfont_note_selections) {
            fail(
                "soundfont_note_selections",
                "requires controls.soundfont_by_note"
            );
        }
    } else {
        requireSequence(
            soundfont_note_selections,
            "soundfont_note_selections"
        );
        if (soundfont_note_selections.size() == 0) {
            fail(
                "soundfont_note_selections",
                "must contain at least one entry"
            );
        }

        std::unordered_set<int> selection_keys;
        for (std::size_t index = 0;
             index < soundfont_note_selections.size();
             ++index) {
            const auto node = soundfont_note_selections[index];
            const std::string location = "soundfont_note_selections["
                + std::to_string(index) + "]";
            rejectUnknownKeys(node, location, {"channel", "key", "soundfont"});

            const int channel = boundedInt(node, "channel", location, 1, 16) - 1;
            const int key = boundedInt(node, "key", location, 0, 127);
            const auto soundfont = nonEmptyString(node, "soundfont", location);
            const auto found = soundfont_indices.find(soundfont);
            if (found == soundfont_indices.end()) {
                fail(
                    location + ".soundfont",
                    "unknown SoundFont id '" + soundfont + "'"
                );
            }

            const int selection_key = channel * 128 + key;
            if (!selection_keys.insert(selection_key).second) {
                fail(location, "duplicate channel and key mapping");
            }

            config.soundfont_note_selections.push_back({
                .channel = channel,
                .key = key,
                .soundfont_index = found->second,
            });
        }
    }

    struct NamedControl {
        std::string_view name;
        const MidiControlBinding* binding;
    };
    std::vector<NamedControl> actions{
        NamedControl{"recording", &config.recording_control},
        NamedControl{"octave_down", &config.octave_down_control},
        NamedControl{"octave_up", &config.octave_up_control},
    };
    if (config.next_soundfont_control) {
        actions.push_back({"next_soundfont", &*config.next_soundfont_control});
    }
    if (config.soundfont_by_note_control) {
        actions.push_back({
            "soundfont_by_note",
            &*config.soundfont_by_note_control,
        });
    }

    for (std::size_t index = 0;
         index < config.soundfont_note_selections.size();
         ++index) {
        const auto& selection = config.soundfont_note_selections[index];
        const MidiMessage selection_note{
            .channel = selection.channel,
            .key = selection.key,
            .velocity = 1,
        };
        for (const auto& action : actions) {
            if (action.binding->matches(
                    MidiMessageType::NoteOn,
                    selection_note
                )) {
                fail(
                    "soundfont_note_selections[" + std::to_string(index) + "]",
                    "channel and key overlap controls."
                        + std::string{action.name}
                );
            }
        }
    }

    for (std::size_t first = 0; first < actions.size(); ++first) {
        for (std::size_t second = first + 1; second < actions.size(); ++second) {
            if (actions[first].binding->overlaps(*actions[second].binding)) {
                fail(
                    "controls",
                    std::string{actions[first].name} + " and "
                        + std::string{actions[second].name}
                        + " bindings must not overlap"
                );
            }
        }
    }

    return config;
}

} // namespace zeta
