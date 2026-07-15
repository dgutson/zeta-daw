#pragma once

#include "midi_control_change_mapping.hpp"
#include "midi_event.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace zeta {

class MidiInput {
public:
    using Handler = std::function<void(MidiEvent)>;

    virtual ~MidiInput() = default;

    virtual void start(
        std::vector<MidiControlChangeMapping> mappings,
        Handler handler
    ) = 0;
    virtual void stop() noexcept = 0;
};

std::unique_ptr<MidiInput> makeMidiInput();

} // namespace zeta
