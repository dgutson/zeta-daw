#pragma once

#include "midi_event.hpp"

#include <functional>
#include <memory>

namespace zeta {

class MidiInput {
public:
    using Handler = std::function<void(MidiEvent)>;

    virtual ~MidiInput() = default;

    virtual void start(Handler handler) = 0;
    virtual void stop() noexcept = 0;
};

std::unique_ptr<MidiInput> makeMidiInput();

} // namespace zeta
