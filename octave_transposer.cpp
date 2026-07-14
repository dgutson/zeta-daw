#include "octave_transposer.hpp"

#include <algorithm>

namespace zeta {
namespace {

constexpr int minimum_octaves = -3;
constexpr int maximum_octaves = 4;
constexpr int semitones_per_octave = 12;
constexpr int minimum_midi_key = 0;
constexpr int maximum_midi_key = 127;

bool hasKey(MidiMessageType type) noexcept {
    return type == MidiMessageType::NoteOff
        || type == MidiMessageType::NoteOn
        || type == MidiMessageType::PolyphonicKeyPressure;
}

} // namespace

void OctaveTransposer::octaveDown() noexcept {
    octaves_ = std::max(octaves_ - 1, minimum_octaves);
}

void OctaveTransposer::octaveUp() noexcept {
    octaves_ = std::min(octaves_ + 1, maximum_octaves);
}

std::optional<MidiMessage> OctaveTransposer::transpose(
    const MidiMessage& message
) const noexcept {
    if (!hasKey(classifyMidiMessage(message.raw_type))) {
        return message;
    }

    const int transposed_key = message.key + octaves_ * semitones_per_octave;
    if (transposed_key < minimum_midi_key
        || transposed_key > maximum_midi_key) {
        return std::nullopt;
    }

    MidiMessage transposed = message;
    transposed.key = transposed_key;
    return transposed;
}

} // namespace zeta
