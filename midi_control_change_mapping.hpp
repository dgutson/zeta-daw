#pragma once

#include "midi_event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace zeta {

struct MidiControlChangeMapping {
    std::string source_port;
    int channel{};
    int controller{};
    int target_controller{};
};

class MidiControlChangeMapper final {
public:
    MidiControlChangeMapper(
        std::string_view source_port,
        std::span<const MidiControlChangeMapping> mappings
    ) noexcept;

    MidiEvent map(MidiEvent event) const noexcept;

private:
    static constexpr std::size_t channel_count = 16;
    static constexpr std::size_t controller_count = 128;
    static constexpr std::uint8_t unmatched = 0xFF;

    static constexpr std::size_t mappingIndex(
        int channel,
        int controller
    ) noexcept {
        return static_cast<std::size_t>(channel) * controller_count
            + static_cast<std::size_t>(controller);
    }

    std::array<std::uint8_t, channel_count * controller_count> targets_;
};

} // namespace zeta
