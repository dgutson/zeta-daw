#pragma once

#include "midi_event.hpp"

#include <cstdint>

namespace zeta {

enum class MidiControlType : std::uint8_t {
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

} // namespace zeta
