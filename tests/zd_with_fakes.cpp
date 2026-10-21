// zd built with the fake MIDI input and the fake FluidSynth, for tests that
// run the whole process from main.cpp. This makeMidiInput() takes the place
// of midi_input.cpp's.

#include "fake_midi_input.hpp"

#include <cstdio>
#include <memory>

namespace {

// The tests read standard output through a pipe. C stdio, which std::cout
// writes through, holds pipe output until exit unless it is line buffered.
[[maybe_unused]] const int line_buffered_output =
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);

} // namespace

namespace zeta {

std::unique_ptr<MidiInput> makeMidiInput() {
    return fake_midi_input::makeInput();
}

} // namespace zeta
