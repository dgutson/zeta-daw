#include "midi_control_change_mapping.hpp"

namespace zeta {

MidiControlChangeMapper::MidiControlChangeMapper(
    std::string_view source_port,
    std::span<const MidiControlChangeMapping> mappings
) noexcept {
    targets_.fill(unmatched);

    for (const auto& mapping : mappings) {
        if (mapping.source_port != source_port) {
            continue;
        }

        targets_[mappingIndex(mapping.channel, mapping.controller)]
            = static_cast<std::uint8_t>(mapping.target_controller);
    }
}

MidiEvent MidiControlChangeMapper::map(MidiEvent event) const noexcept {
    if (event.type != MidiMessageType::ControlChange) {
        return event;
    }

    const auto target = targets_[mappingIndex(
        event.message.channel,
        event.message.control
    )];
    if (target != unmatched) {
        event.message.control = target;
    }
    return event;
}

} // namespace zeta
