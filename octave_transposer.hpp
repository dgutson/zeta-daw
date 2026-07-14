#pragma once

#include "midi_event.hpp"

#include <optional>

namespace zeta {

class OctaveTransposer final {
public:
    void octaveDown() noexcept;
    void octaveUp() noexcept;

    std::optional<MidiMessage> transpose(
        const MidiMessage& message
    ) const noexcept;

private:
    int octaves_{};
};

} // namespace zeta
