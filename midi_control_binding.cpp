#include "midi_control_binding.hpp"

namespace zeta {

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

} // namespace zeta
