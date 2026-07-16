#pragma once

#include <fluidsynth.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace fake_fluidsynth {

enum class CallKind {
    ConfigureMidiChannels,
    LoadSoundFont,
    SelectProgram,
    HandleMidi,
    SynthNoteOn,
    SynthNoteOff,
    SynthControlChange,
    DeleteAudioDriver,
    DeleteSynth,
    DeleteSettings,
};

struct Call {
    CallKind kind{};
    int type{};
    int channel{};
    int key{};
    int velocity{};
    int control{};
    int value{};
    int soundfont_id{};
    int bank{};
    int preset{};
    std::string text;
};

void reset();
std::vector<Call> calls();

bool waitUntil(
    const std::function<bool(const std::vector<Call>&)>& predicate,
    std::chrono::milliseconds timeout = std::chrono::seconds(1)
);

} // namespace fake_fluidsynth
