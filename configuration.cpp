#include "configuration.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string_view>
#include <unordered_set>

namespace zeta {
namespace {

constexpr int required_schema_version = 3;

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
        "soundfonts",
        "controls",
    });

    if (required<int>(root, "schema_version", "configuration")
        != required_schema_version) {
        fail("configuration.schema_version", "unsupported schema version");
    }

    ApplicationConfig config;
    const auto soundfonts = root["soundfonts"];
    requireSequence(soundfonts, "soundfonts");
    if (soundfonts.size() == 0) {
        fail("soundfonts", "must contain at least one entry");
    }

    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < soundfonts.size(); ++index) {
        const auto node = soundfonts[index];
        const std::string location = "soundfonts[" + std::to_string(index) + "]";
        rejectUnknownKeys(node, location, {"id", "file", "bank", "preset"});

        auto id = nonEmptyString(node, "id", location);
        if (!ids.insert(id).second) {
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
    rejectUnknownKeys(controls, "controls", {"recording", "next_soundfont"});
    config.recording_control = parseActionControl(controls, "recording");
    config.next_soundfont_control = parseActionControl(
        controls,
        "next_soundfont"
    );

    if (config.recording_control.overlaps(config.next_soundfont_control)) {
        fail(
            "controls",
            "recording and next_soundfont bindings must not overlap"
        );
    }

    return config;
}

} // namespace zeta
