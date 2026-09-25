#include "integration_support.hpp"

#include <algorithm>
#include <vector>

namespace integration_support {

namespace gs = hegel::generators;

using fake_fluidsynth::Call;
using fake_fluidsynth::CallKind;
using zeta::ApplicationConfig;
using zeta::LoopSlotDefinition;
using zeta::MidiControlBinding;
using zeta::MidiControlType;
using zeta::MidiMessage;
using zeta::MidiMessageType;
using zeta::SoundFontDefinition;

ApplicationConfig testConfig() {
    return {
        .audio = {},
        .loop_slots = {
            LoopSlotDefinition{.key = first_slot_key},
            LoopSlotDefinition{.key = second_slot_key},
        },
        .soundfonts = {
            SoundFontDefinition{
                .id = "piano",
                .file = "piano.sf2",
                .bank = 0,
                .preset = 0,
                .key = 71,
            },
            SoundFontDefinition{
                .id = "bass",
                .file = "bass.sf2",
                .bank = 0,
                .preset = 34,
                .key = 72,
            },
        },
        .midi_control_change_mappings = {},
        .loop_slot_by_note_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x05,
        },
        .next_soundfont_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x01,
        },
        .soundfont_by_note_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x09,
        },
        .octave_down_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x02,
        },
        .octave_up_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x06,
        },
    };
}

ApplicationConfig threeSlotConfig() {
    auto config = testConfig();
    config.loop_slots.push_back(LoopSlotDefinition{.key = third_slot_key});
    return config;
}

MidiMessage recordedNote(MidiMessageType type, int key, int velocity) {
    return {
        .raw_type = raw(type),
        .key = key,
        .velocity = velocity,
    };
}

std::size_t callCount(CallKind kind, int channel, int key) {
    return static_cast<std::size_t>(std::ranges::count_if(
        fake_fluidsynth::calls(),
        [&](const Call& call) {
            return call.kind == kind
                && call.channel == channel
                && call.key == key;
        }
    ));
}

std::size_t controlChangeCount(int channel, int control) {
    return static_cast<std::size_t>(std::ranges::count_if(
        fake_fluidsynth::calls(),
        [&](const Call& call) {
            return call.kind == CallKind::SynthControlChange
                && call.channel == channel
                && call.control == control;
        }
    ));
}

bool waitForNoteCount(
    int channel,
    int key,
    std::size_t count,
    std::chrono::milliseconds timeout
) {
    return fake_fluidsynth::waitUntil(
        [&](const std::vector<Call>& calls) {
            return static_cast<std::size_t>(std::ranges::count_if(
                calls,
                [&](const Call& call) {
                    return call.kind == CallKind::SynthNoteOn
                        && call.channel == channel
                        && call.key == key;
                }
            )) >= count;
        },
        timeout
    );
}

bool waitForNoteOffCount(
    int channel,
    int key,
    std::size_t count,
    std::chrono::milliseconds timeout
) {
    return fake_fluidsynth::waitUntil(
        [&](const std::vector<Call>& calls) {
            return static_cast<std::size_t>(std::ranges::count_if(
                calls,
                [&](const Call& call) {
                    return call.kind == CallKind::SynthNoteOff
                        && call.channel == channel
                        && call.key == key;
                }
            )) >= count;
        },
        timeout
    );
}

std::chrono::microseconds drawDuration(
    hegel::TestCase& tc,
    std::string_view name,
    int shortest_microseconds,
    int longest_microseconds
) {
    return std::chrono::microseconds(tc.draw(
        name,
        gs::integers<int>({
            .min_value = shortest_microseconds,
            .max_value = longest_microseconds,
        }),
        repeatable
    ));
}

} // namespace integration_support
