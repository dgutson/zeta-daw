#pragma once

#include <cstdint>

namespace zeta {

// What a loop slot records: the notes played, or the audio input.
enum class LoopSlotMedium : std::uint8_t {
    Midi,
    Audio,
};

} // namespace zeta
