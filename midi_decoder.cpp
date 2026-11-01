#include "midi_decoder.hpp"

#include <cstddef>

namespace zeta {
namespace {

constexpr std::uint8_t status_mask = 0xF0;
constexpr std::uint8_t channel_mask = 0x0F;
constexpr std::uint8_t system_exclusive = 0xF0;
constexpr std::uint8_t end_of_exclusive = 0xF7;
constexpr std::uint8_t universal_realtime = 0x7F;
constexpr std::uint8_t machine_control = 0x06;

template<std::size_t Count>
bool hasValidDataBytes(std::span<const std::uint8_t> bytes) noexcept {
    static_assert(Count == 1 || Count == 2);

    if (bytes.size() < Count + 1) {
        return false;
    }

    if (bytes[1] >= 0x80) {
        return false;
    }

    if constexpr (Count == 2) {
        return bytes[2] < 0x80;
    }
    return true;
}

} // namespace

std::optional<MidiEvent> decodeMidiEvent(
    std::span<const std::uint8_t> bytes
) noexcept {
    if (bytes.empty()) {
        return std::nullopt;
    }

    const auto status = bytes.front();
    if (status == system_exclusive) {
        MidiEvent event{
            .type = MidiMessageType::Other,
            .message = {.raw_type = system_exclusive},
        };

        if (bytes.size() >= 6
            && bytes[1] == universal_realtime
            && bytes[3] == machine_control
            && bytes.back() == end_of_exclusive) {
            event.type = MidiMessageType::MachineControl;
            event.message.device_id = bytes[2];
            event.message.machine_control_command = bytes[4];
        }
        return event;
    }

    if (status < 0x80) {
        return std::nullopt;
    }

    const int raw_type = status & status_mask;
    const auto type = classifyMidiMessage(raw_type);
    MidiEvent event{
        .type = type,
        .message = {
            .raw_type = raw_type,
            .channel = status & channel_mask,
        },
    };

    switch (type) {
    case MidiMessageType::NoteOff:
    case MidiMessageType::NoteOn:
        if (!hasValidDataBytes<2>(bytes)) {
            return std::nullopt;
        }
        event.message.key = bytes[1];
        event.message.velocity = bytes[2];
        break;
    case MidiMessageType::PolyphonicKeyPressure:
        if (!hasValidDataBytes<2>(bytes)) {
            return std::nullopt;
        }
        event.message.key = bytes[1];
        event.message.pressure = bytes[2];
        break;
    case MidiMessageType::ControlChange:
        if (!hasValidDataBytes<2>(bytes)) {
            return std::nullopt;
        }
        event.message.control = bytes[1];
        event.message.value = bytes[2];
        break;
    case MidiMessageType::ProgramChange:
        if (!hasValidDataBytes<1>(bytes)) {
            return std::nullopt;
        }
        event.message.program = bytes[1];
        break;
    case MidiMessageType::ChannelPressure:
        if (!hasValidDataBytes<1>(bytes)) {
            return std::nullopt;
        }
        event.message.pressure = bytes[1];
        break;
    case MidiMessageType::PitchBend:
        if (!hasValidDataBytes<2>(bytes)) {
            return std::nullopt;
        }
        event.message.pitch = bytes[1] | (bytes[2] << 7);
        break;
    case MidiMessageType::MachineControl:
        break;
    case MidiMessageType::Other:
        event.message.raw_type = status;
        break;
    }

    return event;
}

} // namespace zeta
