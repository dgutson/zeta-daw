#pragma once

#include "configuration.hpp"
#include "midi_event.hpp"

#include <memory>

namespace zeta {

class SynthEngine final {
public:
    explicit SynthEngine(const ApplicationConfig& config);
    ~SynthEngine();

    SynthEngine(const SynthEngine&) = delete;
    SynthEngine& operator=(const SynthEngine&) = delete;

    int send(const MidiMessage& message, int channel);
    int noteOn(int channel, int key, int velocity);
    int noteOff(int channel, int key);

    void select(const SoundFontDefinition& soundfont, int channel);
    void allNotesOff(int channel);
    void allNotesOff();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeta
