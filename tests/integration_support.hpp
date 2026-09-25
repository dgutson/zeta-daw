#pragma once

#include "fake_fluidsynth.hpp"
#include "../configuration.hpp"
#include "../midi_event.hpp"

#include <hegel/hegel.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <streambuf>
#include <string_view>

// Configuration, MIDI, and FluidSynth call-log helpers shared by the tests
// that run real slot workers against the fake FluidSynth.
namespace integration_support {

constexpr int first_slot_key = 48;
constexpr int second_slot_key = 50;
constexpr int third_slot_key = 52;
constexpr int first_slot_channel = 1;
constexpr int second_slot_channel = 2;
constexpr int third_slot_channel = 3;
constexpr int all_notes_off_controller = 123;
// Rules run many times per case, so their draws print numbered.
constexpr bool repeatable = true;

constexpr int raw(zeta::MidiMessageType type) {
    return static_cast<int>(type);
}

zeta::ApplicationConfig testConfig();
zeta::ApplicationConfig threeSlotConfig();

zeta::MidiMessage recordedNote(
    zeta::MidiMessageType type,
    int key,
    int velocity = 0
);

std::size_t callCount(fake_fluidsynth::CallKind kind, int channel, int key);
std::size_t controlChangeCount(int channel, int control);

bool waitForNoteCount(
    int channel,
    int key,
    std::size_t count,
    std::chrono::milliseconds timeout = std::chrono::seconds(1)
);
bool waitForNoteOffCount(
    int channel,
    int key,
    std::size_t count,
    std::chrono::milliseconds timeout = std::chrono::seconds(1)
);

class DiscardedStandardOutput final {
public:
    DiscardedStandardOutput() : previous_(std::cout.rdbuf(&discarded_)) {}

    ~DiscardedStandardOutput() {
        std::cout.rdbuf(previous_);
    }

    DiscardedStandardOutput(const DiscardedStandardOutput&) = delete;
    DiscardedStandardOutput& operator=(const DiscardedStandardOutput&) = delete;

private:
    class DiscardingBuffer final : public std::streambuf {
    protected:
        int_type overflow(int_type character) override {
            return traits_type::not_eof(character);
        }
    };

    DiscardingBuffer discarded_;
    std::streambuf* previous_;
};

std::chrono::microseconds drawDuration(
    hegel::TestCase& tc,
    std::string_view name,
    int shortest_microseconds,
    int longest_microseconds
);

} // namespace integration_support
