#include "midi_event.hpp"

namespace zeta {

MidiMessageType classifyMidiMessage(int raw_type) noexcept {
    switch (raw_type) {
    case static_cast<int>(MidiMessageType::NoteOff):
        return MidiMessageType::NoteOff;
    case static_cast<int>(MidiMessageType::NoteOn):
        return MidiMessageType::NoteOn;
    case static_cast<int>(MidiMessageType::PolyphonicKeyPressure):
        return MidiMessageType::PolyphonicKeyPressure;
    case static_cast<int>(MidiMessageType::ControlChange):
        return MidiMessageType::ControlChange;
    case static_cast<int>(MidiMessageType::ProgramChange):
        return MidiMessageType::ProgramChange;
    case static_cast<int>(MidiMessageType::ChannelPressure):
        return MidiMessageType::ChannelPressure;
    case static_cast<int>(MidiMessageType::PitchBend):
        return MidiMessageType::PitchBend;
    default:
        return MidiMessageType::Other;
    }
}

} // namespace zeta
