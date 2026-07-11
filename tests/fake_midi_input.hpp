#pragma once

#include "../midi_input.hpp"

#include <memory>

namespace fake_midi_input {

struct MidiEvent {
    int type{};
    int channel{};
    int key{};
    int velocity{};
    int control{};
    int value{};
    int program{};
    int pitch{};
    int pressure{};
};

std::unique_ptr<zeta::MidiInput> makeInput();
void reset();
int emitMidi(const MidiEvent& event);

} // namespace fake_midi_input
