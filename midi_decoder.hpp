#pragma once

#include "midi_event.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace zeta {

std::optional<MidiEvent> decodeMidiEvent(std::span<const std::uint8_t> bytes) noexcept;

} // namespace zeta
