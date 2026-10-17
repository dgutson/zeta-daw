#pragma once

#include <cstddef>
#include <vector>

// The scripted one-minute performance that the audio test plays through the
// real Application: a live melody over a one-bar guide and a two-bar regular
// loop, with the SoundFont, octave and slot controls a performer uses.
namespace audio_performance {

enum class ActionKind {
    NoteOn,
    NoteOff,
    LoopSlotControl,
    NextSoundFont,
    OctaveDown,
    OctaveUp,
    Shutdown,
};

struct Step {
    double at_ms{};
    ActionKind kind{};
    int key{};
    int value{};
};

// A guide take's first Note On and its completion, as indexes into
// Performance::steps. Zeta sets the guide period from the times it handles
// these two steps, truncated to whole milliseconds.
struct GuideTake {
    std::size_t first_note{};
    std::size_t completion{};
};

struct Performance {
    // Sorted by time; the last one is Shutdown.
    std::vector<Step> steps;
    std::vector<GuideTake> guide_takes;
};

inline constexpr int guide_slot_key = 48;
inline constexpr int regular_slot_key = 50;

Performance performance();

// Time after Shutdown that the capture keeps recording the release tails.
inline constexpr double silence_after_shutdown_ms = 1500.0;

} // namespace audio_performance
