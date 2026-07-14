#pragma once

#include "midi_event.hpp"

namespace zeta {

class OctaveTransposer final {
public:
    void octaveDown() noexcept;
    void octaveUp() noexcept;

    MidiMessage transpose(const MidiMessage& message) const noexcept;

private:
    static bool hasKey(MidiMessageType type) noexcept {
        return type == MidiMessageType::NoteOff
            || type == MidiMessageType::NoteOn
            || type == MidiMessageType::PolyphonicKeyPressure;
    }

    int octaves_{};
};

} // namespace zeta
