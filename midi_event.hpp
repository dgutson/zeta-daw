#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace zeta {

enum class MidiMessageType : int {
    Other = -1,
    NoteOff = 0x80,
    NoteOn = 0x90,
    PolyphonicKeyPressure = 0xA0,
    ControlChange = 0xB0,
    ProgramChange = 0xC0,
    ChannelPressure = 0xD0,
    PitchBend = 0xE0,
    MachineControl = 0x100,
};

struct MidiMessage {
    int raw_type{};
    int channel{};
    int key{};
    int velocity{};
    int control{};
    int value{};
    int program{};
    int pitch{};
    int pressure{};
    int device_id{};
    int machine_control_command{};
};

struct MidiEvent {
    MidiMessageType type{MidiMessageType::Other};
    MidiMessage message;
};

MidiMessageType classifyMidiMessage(int raw_type) noexcept;
std::optional<MidiEvent> decodeMidiEvent(std::span<const std::uint8_t> bytes) noexcept;

} // namespace zeta
