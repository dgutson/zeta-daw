#include "fake_fluidsynth.hpp"
#include "integration_support.hpp"
#include "../loop_slot_group.hpp"
#include "../synth_engine.hpp"

#include <gtest/gtest.h>
#include <hegel/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace integration_support;
using fake_fluidsynth::Call;
using fake_fluidsynth::CallKind;
using zeta::ApplicationConfig;
using zeta::LoopSlotGroup;
using zeta::LoopSlotSelectionOutcome;
using zeta::LooperClock;
using zeta::Milliseconds;
using zeta::MidiMessage;
using zeta::MidiMessageType;
using zeta::OctaveTransposer;
using zeta::RecordedNoteKind;
using zeta::SlotId;
using zeta::SoundFontDefinition;
using zeta::SynthEngine;
using zeta::TakeTiming;
using zeta::TimePoint;

constexpr int direct_guide_key = 72;

void completeDirectGuide(
    LoopSlotGroup& slots,
    const SoundFontDefinition& soundfont,
    const OctaveTransposer& transposer,
    TimePoint first_cycle_at,
    Milliseconds period
) {
    const auto guide = slots.requestSelection(
        first_slot_key,
        soundfont,
        transposer
    );
    ASSERT_EQ(guide.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(
        guide.id,
        RecordedNoteKind::NoteOn,
        recordedNote(MidiMessageType::NoteOn, direct_guide_key, 100),
        0ms
    );
    slots.recordNote(
        guide.id,
        RecordedNoteKind::NoteOff,
        recordedNote(MidiMessageType::NoteOff, direct_guide_key),
        0ms
    );
    slots.completeRecording(guide.id, TakeTiming{
        .recording_started_at = first_cycle_at - period,
        .completed_at = first_cycle_at,
    });
}

std::string describeCall(const Call& call) {
    switch (call.kind) {
    case CallKind::SynthControlChange:
        return "CC " + std::to_string(call.control)
            + " = " + std::to_string(call.value);
    case CallKind::SelectProgram:
        return "program select sfid " + std::to_string(call.soundfont_id)
            + " bank " + std::to_string(call.bank)
            + " preset " + std::to_string(call.preset);
    case CallKind::SynthNoteOn:
        return "Note On " + std::to_string(call.key);
    case CallKind::SynthNoteOff:
        return "Note Off " + std::to_string(call.key);
    default:
        return "call kind " + std::to_string(static_cast<int>(call.kind));
    }
}

// The calls on `channel` from call-log position `from` up to and including
// the first Note On. The fake logs each synth call a second time as
// HandleMidi; those copies are skipped.
std::vector<std::string> channelCallsUntilNoteOn(
    std::size_t from,
    int channel
) {
    const auto calls = fake_fluidsynth::calls();
    std::vector<std::string> described;
    for (std::size_t index = from; index < calls.size(); ++index) {
        const auto& call = calls[index];
        if (call.channel != channel || call.kind == CallKind::HandleMidi) {
            continue;
        }
        described.push_back(describeCall(call));
        if (call.kind == CallKind::SynthNoteOn) {
            break;
        }
    }
    return described;
}

class LoopSlotGroupTest : public ::testing::Test {
protected:
    void SetUp() override {
        fake_fluidsynth::reset();
    }
};

TEST_F(LoopSlotGroupTest, LateDependentDispatchKeepsNextGuideDeadline) {
    auto config = testConfig();
    SynthEngine synth_engine{config};
    LoopSlotGroup slots{config.loop_slots, synth_engine};
    OctaveTransposer transposer;
    const auto& soundfont = config.soundfonts.front();
    const MidiMessage note_on{
        .raw_type = raw(MidiMessageType::NoteOn),
        .key = 64,
        .velocity = 100,
    };
    const MidiMessage note_off{
        .raw_type = raw(MidiMessageType::NoteOff),
        .key = 64,
    };

    constexpr auto guide_period = 2000ms;
    constexpr auto dispatch_lateness = 1500ms;
    static_assert(dispatch_lateness < guide_period);
    const auto guide_first_cycle_at = LooperClock::now() - dispatch_lateness;

    const auto guide = slots.requestSelection(
        first_slot_key,
        soundfont,
        transposer
    );
    ASSERT_EQ(guide.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(guide.id, RecordedNoteKind::NoteOn, note_on, 0ms);
    slots.recordNote(guide.id, RecordedNoteKind::NoteOff, note_off, 0ms);
    slots.completeRecording(guide.id, TakeTiming{
        .recording_started_at = guide_first_cycle_at - guide_period,
        .completed_at = guide_first_cycle_at,
    });

    const auto dependent = slots.requestSelection(
        second_slot_key,
        soundfont,
        transposer
    );
    ASSERT_EQ(dependent.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(dependent.id, RecordedNoteKind::NoteOn, note_on, 0ms);
    slots.recordNote(dependent.id, RecordedNoteKind::NoteOff, note_off, 0ms);
    slots.completeRecording(dependent.id, TakeTiming{
        .recording_started_at = guide_first_cycle_at - guide_period,
        .completed_at = guide_first_cycle_at,
    });

    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 1));
    EXPECT_TRUE(waitForNoteCount(second_slot_channel, 64, 2, 1200ms));
}

TEST_F(
    LoopSlotGroupTest,
    RegularFirstCycleSkipsExpiredPrefixAndIncludesCompletionBoundary
) {
    auto config = testConfig();
    SynthEngine synth_engine{config};
    LoopSlotGroup slots{config.loop_slots, synth_engine};
    OctaveTransposer transposer;
    const auto& soundfont = config.soundfonts.front();

    constexpr auto guide_period = 400ms;
    constexpr auto elapsed_repetition = 80ms;
    const auto completed_at = LooperClock::now();
    const auto first_cycle_at = completed_at - elapsed_repetition;
    const auto recording_started_at = first_cycle_at - guide_period;
    completeDirectGuide(
        slots,
        soundfont,
        transposer,
        completed_at - guide_period / 2,
        guide_period
    );

    const auto regular = slots.requestSelection(
        second_slot_key,
        soundfont,
        transposer
    );
    ASSERT_EQ(regular.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOn,
        recordedNote(MidiMessageType::NoteOn, 64, 100),
        0ms
    );
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOn,
        recordedNote(MidiMessageType::NoteOn, 67, 100),
        elapsed_repetition
    );
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOff,
        recordedNote(MidiMessageType::NoteOff, 64),
        140ms
    );
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOff,
        recordedNote(MidiMessageType::NoteOff, 67),
        180ms
    );
    const auto silence_count = controlChangeCount(second_slot_channel, 123);

    slots.completeRecording(regular.id, TakeTiming{
        .recording_started_at = recording_started_at,
        .completed_at = completed_at,
    });

    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 67, 1, 200ms));
    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, second_slot_channel, 64),
        0U
    );
    ASSERT_TRUE(waitForNoteOffCount(second_slot_channel, 64, 1, 200ms));
    EXPECT_GT(controlChangeCount(second_slot_channel, 123), silence_count);

    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 1, 500ms));
    EXPECT_TRUE(waitForNoteCount(second_slot_channel, 67, 2, 200ms));
}

TEST_F(LoopSlotGroupTest, RegularWaitsWhenCurrentRepetitionEventsArePast) {
    auto config = testConfig();
    SynthEngine synth_engine{config};
    LoopSlotGroup slots{config.loop_slots, synth_engine};
    OctaveTransposer transposer;
    const auto& soundfont = config.soundfonts.front();

    constexpr auto guide_period = 400ms;
    constexpr auto elapsed_repetition = 180ms;
    const auto completed_at = LooperClock::now();
    const auto first_cycle_at = completed_at - elapsed_repetition;
    const auto recording_started_at = first_cycle_at - guide_period;
    completeDirectGuide(
        slots,
        soundfont,
        transposer,
        completed_at - guide_period / 2,
        guide_period
    );

    const auto regular = slots.requestSelection(
        second_slot_key,
        soundfont,
        transposer
    );
    ASSERT_EQ(regular.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOn,
        recordedNote(MidiMessageType::NoteOn, 64, 100),
        0ms
    );
    slots.recordNote(
        regular.id,
        RecordedNoteKind::NoteOff,
        recordedNote(MidiMessageType::NoteOff, 64),
        100ms
    );

    slots.completeRecording(regular.id, TakeTiming{
        .recording_started_at = recording_started_at,
        .completed_at = completed_at,
    });

    std::this_thread::sleep_for(75ms);
    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, second_slot_channel, 64),
        0U
    );
    EXPECT_TRUE(waitForNoteCount(second_slot_channel, 64, 1, 400ms));
    EXPECT_TRUE(waitForNoteOffCount(second_slot_channel, 64, 1, 200ms));
}

TEST_F(
    LoopSlotGroupTest,
    CompletionSilencesAndSelectsLockedProgramBeforePlayback
) {
    auto config = testConfig();
    SynthEngine synth_engine{config};
    LoopSlotGroup slots{config.loop_slots, synth_engine};
    OctaveTransposer transposer;

    const auto guide = slots.requestSelection(
        first_slot_key,
        config.soundfonts.front(),
        transposer
    );
    ASSERT_EQ(guide.outcome, LoopSlotSelectionOutcome::Armed);
    slots.recordNote(
        guide.id,
        RecordedNoteKind::NoteOn,
        recordedNote(MidiMessageType::NoteOn, 72, 100),
        0ms
    );
    const auto completion_calls_from = fake_fluidsynth::calls().size();

    constexpr auto guide_period = 400ms;
    const auto completed_at = LooperClock::now();
    slots.completeRecording(guide.id, TakeTiming{
        .recording_started_at = completed_at - guide_period,
        .completed_at = completed_at,
    });

    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 72, 1));
    // The fake numbers SoundFonts from 1 in load order, so the piano is 1.
    EXPECT_EQ(
        channelCallsUntilNoteOn(completion_calls_from, first_slot_channel),
        (std::vector<std::string>{
            "CC 64 = 0",  // sustain off
            "CC 123 = 0", // all notes off
            "program select sfid 1 bank 0 preset 0",
            "Note On 72",
        })
    );
}

// Completing the guide in the millisecond of its first note gives period
// zero. The guide then counts as looping but plays nothing.
TEST_F(LoopSlotGroupTest, ZeroLengthGuideTakeLoopsSilentlyUntilStopped) {
    auto config = testConfig();
    SynthEngine synth_engine{config};
    LoopSlotGroup slots{config.loop_slots, synth_engine};
    OctaveTransposer transposer;
    const auto& soundfont = config.soundfonts.front();

    completeDirectGuide(
        slots,
        soundfont,
        transposer,
        LooperClock::now(),
        Milliseconds::zero()
    );

    // A zero-length take that played would sound its first note at once.
    constexpr auto silence_wait = 50ms;
    EXPECT_FALSE(waitForNoteCount(
        first_slot_channel,
        direct_guide_key,
        1,
        silence_wait
    ));
    EXPECT_EQ(
        slots.requestSelection(second_slot_key, soundfont, transposer).outcome,
        LoopSlotSelectionOutcome::GuideRequired
    );
    EXPECT_EQ(
        slots.requestSelection(first_slot_key, soundfont, transposer).outcome,
        LoopSlotSelectionOutcome::Stopped
    );

    constexpr auto guide_period = 400ms;
    completeDirectGuide(
        slots,
        soundfont,
        transposer,
        LooperClock::now(),
        guide_period
    );
    EXPECT_TRUE(waitForNoteCount(first_slot_channel, direct_guide_key, 1));
}

namespace gs = hegel::generators;

constexpr SlotId guide_slot = 0;
constexpr int midi_key_count = 128;
constexpr int take_note_velocity = 100;
constexpr int longest_note_gap_ms = 3;
// A positive note length keeps every guide period playable.
constexpr int shortest_note_ms = 1;
constexpr int longest_note_ms = 3;
constexpr int longest_completion_delay_ms = 3;
constexpr int longest_playback_pause_microseconds = 3000;
constexpr std::int64_t slot_command_steps = 30;

enum class SlotPhase {
    Muted,
    Armed,
    Looping,
};

struct SlotModel {
    SlotPhase phase{SlotPhase::Muted};
    // Keys of the armed or looping take.
    std::vector<int> keys;
};

struct SlotGroupModel {
    std::vector<SlotModel> slots;
    // Offset of the armed take's last Note Off.
    Milliseconds recorded_until{};
    // Keys are handed out in order, so no two takes share a key.
    int next_key{};
};

const char* phaseName(SlotPhase phase) {
    switch (phase) {
    case SlotPhase::Muted:
        return "Muted";
    case SlotPhase::Armed:
        return "Armed";
    case SlotPhase::Looping:
        return "Looping";
    }
    return "?";
}

std::ostream& operator<<(std::ostream& out, const SlotGroupModel& model) {
    out << '{';
    for (std::size_t id = 0; id < model.slots.size(); ++id) {
        const auto& slot = model.slots[id];
        out << (id == 0 ? "" : ", ") << "slot " << id << ": "
            << phaseName(slot.phase);
        for (const int key : slot.keys) {
            out << ' ' << key;
        }
    }
    return out << "; recorded until " << model.recorded_until.count() << "ms}";
}

struct StoppedTake {
    int channel{};
    std::vector<int> keys;
    // Call-log position of the all-notes-off with which the stop silenced
    // the slot channel.
    std::size_t silenced_at{};
};

Milliseconds drawMilliseconds(
    hegel::TestCase& tc,
    std::string_view name,
    int shortest_ms,
    int longest_ms
) {
    return Milliseconds(tc.draw(
        name,
        gs::integers<int>({.min_value = shortest_ms, .max_value = longest_ms}),
        repeatable
    ));
}

// Commands run on one thread, as the master FSM's mutex serializes them in
// production, while every slot worker plays its take on its own thread.
class LoopSlotStopMachine final
    : public hegel::stateful::StateMachine<
        LoopSlotStopMachine,
        SlotGroupModel
    > {
public:
    LoopSlotStopMachine()
        : StateMachine({.initial_state = SlotGroupModel{}}),
          config_{threeSlotConfig()},
          synth_engine_{config_},
          slots_{config_.loop_slots, synth_engine_} {
        state.slots.resize(config_.loop_slots.size());
    }

    std::vector<hegel::stateful::Rule<LoopSlotStopMachine>> rules() {
        using Rule = hegel::stateful::Rule<LoopSlotStopMachine>;

        return {
            Rule("arm a muted slot", [](hegel::TestCase& tc, auto& machine) {
                machine.armMutedSlot(tc);
            }),
            Rule("record a note", [](hegel::TestCase& tc, auto& machine) {
                machine.recordNote(tc);
            }),
            Rule("complete the take", [](hegel::TestCase& tc, auto& machine) {
                machine.completeTake(tc);
            }),
            Rule("cancel the take", [](hegel::TestCase& tc, auto& machine) {
                machine.cancelTake(tc);
            }),
            Rule("stop a looping slot", [](hegel::TestCase& tc, auto& machine) {
                machine.stopLoopingSlot(tc);
            }),
            Rule("let the loops play", [](hegel::TestCase& tc, auto&) {
                std::this_thread::sleep_for(drawDuration(
                    tc,
                    "pause",
                    0,
                    longest_playback_pause_microseconds
                ));
            }),
        };
    }

    std::vector<hegel::stateful::Invariant<LoopSlotStopMachine>>
    invariants() const {
        using Invariant = hegel::stateful::Invariant<LoopSlotStopMachine>;

        return {
            Invariant("no stopped take sounds again", [](const auto& machine) {
                const auto note = machine.noteAfterStop();
                if (note) {
                    throw std::runtime_error(*note);
                }
            }),
        };
    }

    // Terminating joins every worker, so no take can play afterwards.
    void terminate() {
        slots_.terminateAll();
    }

    std::optional<std::string> noteAfterStop() const {
        const auto calls = fake_fluidsynth::calls();
        for (const auto& take : stopped_takes_) {
            const auto after_silence = calls.begin()
                + static_cast<std::ptrdiff_t>(take.silenced_at) + 1;
            const auto note = std::find_if(
                after_silence,
                calls.end(),
                [&take](const Call& call) {
                    return call.kind == CallKind::SynthNoteOn
                        && call.channel == take.channel
                        && std::ranges::find(take.keys, call.key)
                            != take.keys.end();
                }
            );
            if (note != calls.end()) {
                return "channel " + std::to_string(take.channel)
                    + " played key " + std::to_string(note->key)
                    + " after its take was stopped";
            }
        }
        return std::nullopt;
    }

private:
    // The guide arms whenever it is muted; a regular slot arms only while
    // the guide loops.
    void armMutedSlot(hegel::TestCase& tc) {
        tc.assume(!armedSlot());
        const bool guide_looping =
            state.slots[guide_slot].phase == SlotPhase::Looping;
        std::vector<SlotId> armable;
        for (const auto id : slotsIn(SlotPhase::Muted)) {
            if (id == guide_slot || guide_looping) {
                armable.push_back(id);
            }
        }
        tc.assume(!armable.empty());
        const auto slot = tc.draw("slot", gs::sampled_from(armable), repeatable);

        requireOutcome(select(slot), LoopSlotSelectionOutcome::Armed);
        state.slots[slot] = SlotModel{.phase = SlotPhase::Armed, .keys = {}};
        state.recorded_until = Milliseconds::zero();
    }

    void recordNote(hegel::TestCase& tc) {
        const auto slot = armedSlot();
        tc.assume(slot.has_value() && state.next_key < midi_key_count);
        auto& take = state.slots[*slot];
        // The first note starts the take at offset zero.
        const auto gap = take.keys.empty()
            ? Milliseconds::zero()
            : drawMilliseconds(tc, "gap", 0, longest_note_gap_ms);
        const auto length = drawMilliseconds(
            tc,
            "length",
            shortest_note_ms,
            longest_note_ms
        );
        const int key = state.next_key++;
        const auto pressed_at = state.recorded_until + gap;

        slots_.recordNote(
            *slot,
            RecordedNoteKind::NoteOn,
            recordedNote(MidiMessageType::NoteOn, key, take_note_velocity),
            pressed_at
        );
        slots_.recordNote(
            *slot,
            RecordedNoteKind::NoteOff,
            recordedNote(MidiMessageType::NoteOff, key),
            pressed_at + length
        );
        take.keys.push_back(key);
        state.recorded_until = pressed_at + length;
    }

    // The take is recorded without waiting, so its timing places the
    // recording just before completion.
    void completeTake(hegel::TestCase& tc) {
        const auto slot = armedSlot();
        tc.assume(slot.has_value() && !state.slots[*slot].keys.empty());
        const auto completion_delay = drawMilliseconds(
            tc,
            "completion_delay",
            0,
            longest_completion_delay_ms
        );
        const auto completed_at = LooperClock::now();

        slots_.completeRecording(*slot, TakeTiming{
            .recording_started_at =
                completed_at - (state.recorded_until + completion_delay),
            .completed_at = completed_at,
        });
        state.slots[*slot].phase = SlotPhase::Looping;
    }

    // As in the master FSM, only a take with no notes yet is canceled.
    void cancelTake(hegel::TestCase& tc) {
        const auto slot = armedSlot();
        tc.assume(slot.has_value() && state.slots[*slot].keys.empty());

        slots_.cancelRecording(*slot);
        state.slots[*slot].phase = SlotPhase::Muted;
    }

    void stopLoopingSlot(hegel::TestCase& tc) {
        tc.assume(!armedSlot());
        const auto looping = slotsIn(SlotPhase::Looping);
        tc.assume(!looping.empty());
        const auto slot = tc.draw("slot", gs::sampled_from(looping), repeatable);

        requireOutcome(select(slot), LoopSlotSelectionOutcome::Stopped);
        // Stopping the guide stops every regular slot too.
        const auto stopped = slot == guide_slot
            ? looping
            : std::vector<SlotId>{slot};
        const auto calls = fake_fluidsynth::calls();
        for (const auto id : stopped) {
            const int channel = slots_.channel(id);
            stopped_takes_.push_back({
                .channel = channel,
                .keys = std::move(state.slots[id].keys),
                .silenced_at = lastSilence(calls, channel),
            });
            state.slots[id] = SlotModel{};
        }
    }

    // A stop silences the slot channel under the mutex the worker plays
    // under, so a Note On of the stopped take after that silence was played
    // after the stop. Measuring from the silence, not from the log size
    // when the stop returns, also catches a note played before the test
    // reads the log.
    static std::size_t lastSilence(const std::vector<Call>& calls, int channel) {
        const auto silence = std::find_if(
            calls.rbegin(),
            calls.rend(),
            [channel](const Call& call) {
                return call.kind == CallKind::SynthControlChange
                    && call.channel == channel
                    && call.control == all_notes_off_controller;
            }
        );
        if (silence == calls.rend()) {
            throw std::runtime_error(
                "a stopped slot did not silence its channel"
            );
        }
        return static_cast<std::size_t>(silence.base() - calls.begin()) - 1;
    }

    LoopSlotSelectionOutcome select(SlotId slot) {
        return slots_.requestSelection(
            config_.loop_slots[slot].key,
            config_.soundfonts.front(),
            transposer_
        ).outcome;
    }

    static void requireOutcome(
        LoopSlotSelectionOutcome actual,
        LoopSlotSelectionOutcome expected
    ) {
        if (actual != expected) {
            throw std::runtime_error(
                "the slot group's selection outcome disagrees with the model"
            );
        }
    }

    std::vector<SlotId> slotsIn(SlotPhase phase) const {
        std::vector<SlotId> ids;
        for (SlotId id = 0; id < state.slots.size(); ++id) {
            if (state.slots[id].phase == phase) {
                ids.push_back(id);
            }
        }
        return ids;
    }

    std::optional<SlotId> armedSlot() const {
        const auto armed = slotsIn(SlotPhase::Armed);
        if (armed.empty()) {
            return std::nullopt;
        }
        return armed.front();
    }

    // Slot selection and SynthEngine print to std::cout; discarding it keeps
    // a failing run's report readable.
    DiscardedStandardOutput discarded_output_;
    ApplicationConfig config_;
    SynthEngine synth_engine_;
    LoopSlotGroup slots_;
    OctaveTransposer transposer_;
    std::vector<StoppedTake> stopped_takes_;
};

TEST(LoopSlotGroupPropertyTest, StoppedTakeNeverSoundsAgain) {
    hegel::Settings settings;
    settings.stateful_step_count = slot_command_steps;

    hegel::test([](hegel::TestCase& tc) {
        fake_fluidsynth::reset();
        LoopSlotStopMachine machine;

        hegel::stateful::run(machine, tc);

        machine.terminate();
        ASSERT_EQ(machine.noteAfterStop(), std::nullopt);
    }, settings);
}

} // namespace
