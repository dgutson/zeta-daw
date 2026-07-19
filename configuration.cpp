#include "configuration.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <string_view>
#include <unordered_set>

namespace zeta {
namespace {

constexpr int required_schema_version = 8;
constexpr double minimum_synth_gain = 0.0;
constexpr double maximum_synth_gain = 10.0;

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

int parseControllerKeyName(
    const YAML::Node& node,
    const std::string& location
) {
    constexpr int semitones_per_octave = 12;
    constexpr int configured_octave_to_midi_offset = 2;
    constexpr int maximum_midi_key = 127;
    constexpr std::size_t octave_digit_count = 1;

    // The SE49 convention labels MIDI 60 as C3, so configured octave numbers
    // are two lower than the zero-based octave index used by MIDI arithmetic.
    static constexpr std::array<
        std::string_view,
        semitones_per_octave
    > note_names{
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B",
    };
    if (!node || !node.IsScalar()) {
        fail(location, "expected a controller key name from C0 through G8");
    }

    std::string target;
    try {
        target = node.as<std::string>();
    } catch (const YAML::Exception& error) {
        fail(location, error.what());
    }
    if (target.empty()) {
        fail(location, "expected a controller key name from C0 through G8");
    }

    int semitone = 0;
    for (const auto note : note_names) {
        if (target.size() == note.size() + octave_digit_count
            && target.find(note) == 0) {
            const char octave_digit = target[note.size()];
            if (octave_digit >= '0' && octave_digit <= '9') {
                const int octave = octave_digit - '0';
                const int key =
                    (octave + configured_octave_to_midi_offset)
                    * semitones_per_octave + semitone;
                if (key <= maximum_midi_key) {
                    return key;
                }
            } else {
                fail(
                    location,
                    "expected a controller key name from C0 through G8"
                );
            }
        }
        ++semitone;
    }

    fail(
        location,
        "expected a controller key name from C0 through G8"
    );
}

int parseControllerKey(
    const YAML::Node& node,
    const std::string& location
) {
    return parseControllerKeyName(node["key"], location + ".key");
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

AudioConfig parseAudioConfig(const YAML::Node& node) {
    constexpr std::string_view location{"audio"};
    rejectUnknownKeys(node, std::string{location}, {
        "driver",
        "alsa_device",
        "gain",
    });

    AudioConfig config;
    if (node["driver"]) {
        config.driver = nonEmptyString(node, "driver", std::string{location});
    }
    if (node["alsa_device"]) {
        config.alsa_device = nonEmptyString(
            node,
            "alsa_device",
            std::string{location}
        );
    }
    if (node["gain"]) {
        config.gain = required<double>(node, "gain", std::string{location});
        if (!std::isfinite(config.gain)
            || config.gain < minimum_synth_gain
            || config.gain > maximum_synth_gain) {
            fail(
                "audio.gain",
                "expected a value from 0 to 10"
            );
        }
    }

    if (config.alsa_device && config.driver != "alsa") {
        fail(
            "audio.alsa_device",
            "requires audio.driver to be 'alsa'"
        );
    }

    return config;
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
        "audio",
        "midi_control_change_mappings",
        "loop_slots",
        "soundfonts",
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
    if (root["audio"]) {
        config.audio = parseAudioConfig(root["audio"]);
    }
    const auto midi_control_change_mappings =
        root["midi_control_change_mappings"];
    if (midi_control_change_mappings) {
        config.midi_control_change_mappings = parseMidiControlChangeMappings(
            midi_control_change_mappings
        );
    }

    const auto loop_slots = root["loop_slots"];
    requireSequence(loop_slots, "loop_slots");
    if (loop_slots.size() == 0) {
        fail("loop_slots", "must contain at least one entry");
    }

    std::unordered_set<int> loop_slot_keys;
    for (std::size_t index = 0; index < loop_slots.size(); ++index) {
        const std::string location =
            "loop_slots[" + std::to_string(index) + "]";
        const int key = parseControllerKeyName(loop_slots[index], location);
        if (!loop_slot_keys.insert(key).second) {
            fail(location, "duplicate loop-slot selection key");
        }
        config.loop_slots.push_back({.key = key});
    }

    const auto soundfonts = root["soundfonts"];
    requireSequence(soundfonts, "soundfonts");
    if (soundfonts.size() == 0) {
        fail("soundfonts", "must contain at least one entry");
    }

    std::unordered_set<std::string> soundfont_ids;
    std::unordered_set<int> soundfont_keys;
    for (std::size_t index = 0; index < soundfonts.size(); ++index) {
        const auto node = soundfonts[index];
        const std::string location = "soundfonts[" + std::to_string(index) + "]";
        rejectUnknownKeys(
            node,
            location,
            {"id", "file", "bank", "preset", "key"}
        );

        auto id = nonEmptyString(node, "id", location);
        if (!soundfont_ids.insert(id).second) {
            fail(location + ".id", "duplicate SoundFont id '" + id + "'");
        }

        std::optional<int> key;
        if (node["key"]) {
            key = parseControllerKey(node, location);
            if (!soundfont_keys.insert(*key).second) {
                fail(location + ".key", "duplicate SoundFont selection key");
            }
        }

        config.soundfonts.push_back({
            .id = std::move(id),
            .file = resolveSoundFontPath(
                std::filesystem::absolute(path),
                nonEmptyString(node, "file", location)
            ),
            .bank = boundedInt(node, "bank", location, 0, 16383),
            .preset = boundedInt(node, "preset", location, 0, 127),
            .key = key,
        });
    }

    const auto controls = root["controls"];
    rejectUnknownKeys(controls, "controls", {
        "loop_slot_by_note",
        "next_soundfont",
        "soundfont_by_note",
        "octave_down",
        "octave_up",
    });
    config.loop_slot_by_note_control = parseActionControl(
        controls,
        "loop_slot_by_note"
    );
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

    if (config.soundfont_by_note_control && soundfont_keys.empty()) {
        fail(
            "soundfonts",
            "at least one entry requires key when controls.soundfont_by_note "
            "is configured"
        );
    }
    if (!config.soundfont_by_note_control && !soundfont_keys.empty()) {
        fail(
            "soundfonts",
            "SoundFont keys require controls.soundfont_by_note"
        );
    }

    struct NamedControl {
        std::string_view name;
        const MidiControlBinding* binding;
    };
    std::vector<NamedControl> actions{
        NamedControl{
            "loop_slot_by_note",
            &config.loop_slot_by_note_control,
        },
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

    for (std::size_t index = 0; index < config.loop_slots.size(); ++index) {
        const int loop_slot_key = config.loop_slots[index].key;
        for (const auto& action : actions) {
            if (action.binding->type == MidiControlType::Note
                && action.binding->number == loop_slot_key) {
                fail(
                    "loop_slots[" + std::to_string(index) + "]",
                    "physical key overlaps controls."
                        + std::string{action.name}
                );
            }
        }
    }

    for (std::size_t index = 0; index < config.soundfonts.size(); ++index) {
        const auto& soundfont = config.soundfonts[index];
        if (!soundfont.key) {
            continue;
        }
        const int soundfont_key = soundfont.key.value();
        for (const auto& action : actions) {
            if (action.binding->type == MidiControlType::Note
                && action.binding->number == soundfont_key) {
                fail(
                    "soundfonts[" + std::to_string(index) + "].key",
                    "physical key overlaps controls."
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
