#include "../pending_take.hpp"

#include <gtest/gtest.h>
#include <hegel/gtest.h>

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using zeta::MidiMessage;
using zeta::Milliseconds;
using zeta::PendingTake;
using zeta::RecordedLoopEvent;
using zeta::RecordedNoteKind;

// PendingTake's private fixed event bound.
constexpr std::size_t maximum_event_count = 16384;

MidiMessage note(int key, int velocity = 100) {
    return {
        .key = key,
        .velocity = velocity,
    };
}

TEST(PendingTakeTest, ReleasedNoteDiscardsTrailingCompletionDelay) {
    PendingTake take;
    take.record(RecordedNoteKind::NoteOn, note(60), 0ms);
    take.record(RecordedNoteKind::NoteOff, note(60, 0), 10ms);

    take.finish(30ms);

    ASSERT_EQ(take.events().size(), 2U);
    EXPECT_EQ(take.contentDuration(), 10ms);
    EXPECT_EQ(take.events().back().kind, RecordedNoteKind::NoteOff);
    EXPECT_EQ(take.events().back().time_ms, 10U);
}

TEST(PendingTakeTest, CompletionClosesEveryHeldRecordedKey) {
    PendingTake take;
    take.record(RecordedNoteKind::NoteOn, note(60), 0ms);
    take.record(RecordedNoteKind::NoteOn, note(64), 2ms);
    take.record(RecordedNoteKind::NoteOff, note(60, 0), 5ms);

    take.finish(20ms);

    ASSERT_EQ(take.events().size(), 4U);
    EXPECT_EQ(take.contentDuration(), 20ms);
    EXPECT_EQ(take.events().back().key, 64);
    EXPECT_EQ(take.events().back().kind, RecordedNoteKind::NoteOff);
    EXPECT_EQ(take.events().back().time_ms, 20U);
}

TEST(PendingTakeTest, IgnoresReleaseWithoutAcceptedNoteOn) {
    PendingTake take;

    take.record(RecordedNoteKind::NoteOff, note(60, 0), 12ms);
    take.finish(30ms);

    EXPECT_TRUE(take.events().empty());
    EXPECT_EQ(take.contentDuration(), 0ms);
}

TEST(PendingTakeTest, FixedCapacityAlwaysReservesHeldNoteRelease) {
    PendingTake take;

    for (std::size_t pair = 0; pair < maximum_event_count / 2 - 1; ++pair) {
        take.record(RecordedNoteKind::NoteOn, note(60), 0ms);
        take.record(RecordedNoteKind::NoteOff, note(60, 0), 1ms);
    }
    take.record(RecordedNoteKind::NoteOn, note(64), 2ms);
    take.record(RecordedNoteKind::NoteOn, note(67), 3ms);
    take.finish(4ms);

    ASSERT_EQ(take.events().size(), maximum_event_count);
    EXPECT_EQ(take.events().back().key, 64);
    EXPECT_EQ(take.events().back().kind, RecordedNoteKind::NoteOff);
    EXPECT_EQ(take.events().back().time_ms, 4U);
}

TEST(PendingTakeTest, ResetRetainsReusableEmptyTakeSemantics) {
    PendingTake take;
    take.record(RecordedNoteKind::NoteOn, note(60), 0ms);
    take.finish(5ms);

    take.reset();

    EXPECT_TRUE(take.events().empty());
    EXPECT_EQ(take.contentDuration(), 0ms);
}

namespace gs = hegel::generators;

constexpr int midi_key_count = 128;
// A Note On with velocity zero is a Note Off.
constexpr int lowest_note_on_velocity = 1;
constexpr int lowest_note_off_velocity = 0;
constexpr int highest_velocity = 127;
constexpr int longest_gap_ms = 3;
// A fill stops once at most one position more than its headroom is free, so
// the commands after it meet the event bound.
constexpr std::size_t largest_fill_headroom = 4;
// A released note takes one Note On and one Note Off position.
constexpr std::size_t released_note_events = 2;
constexpr int fill_velocity = 100;
// A fill records about 16,000 events, so short sequences keep the property
// fast; twenty steps still fit a fill, commands at the bound, and completion.
constexpr std::int64_t take_command_steps = 20;
// Rules run many times per case, so their draws print numbered.
constexpr bool repeatable = true;

using KeySet = std::bitset<midi_key_count>;

struct PendingTakeModel {
    // Accepted Note On and Note Off events, in recording order.
    std::vector<RecordedLoopEvent> recorded;
    // Keys whose accepted Note On has no accepted Note Off yet.
    KeySet held;
    // Offset of the latest command; offsets never decrease.
    Milliseconds now{};
    std::optional<Milliseconds> completed_at;
    // Keys still held at completion, which completion must release.
    KeySet released_at_completion;
};

std::vector<int> keysIn(const KeySet& keys) {
    std::vector<int> result;
    for (int key = 0; key < midi_key_count; ++key) {
        if (keys.test(static_cast<std::size_t>(key))) {
            result.push_back(key);
        }
    }
    return result;
}

void printKeys(std::ostream& out, const KeySet& keys) {
    out << '{';
    const char* separator = "";
    for (const int key : keysIn(keys)) {
        out << separator << key;
        separator = ", ";
    }
    out << '}';
}

std::ostream& operator<<(std::ostream& out, const PendingTakeModel& model) {
    out << "{recorded: " << model.recorded.size() << " events, held: ";
    printKeys(out, model.held);
    out << ", now: " << model.now.count() << "ms";
    if (model.completed_at) {
        out << ", completed at " << model.completed_at->count()
            << "ms releasing ";
        printKeys(out, model.released_at_completion);
    }
    return out << '}';
}

bool sameEvent(const RecordedLoopEvent& left, const RecordedLoopEvent& right) {
    return left.time_ms == right.time_ms
        && left.key == right.key
        && left.velocity == right.velocity
        && left.kind == right.kind;
}

class PendingTakeMachine final
    : public hegel::stateful::StateMachine<
        PendingTakeMachine,
        PendingTakeModel
    > {
public:
    PendingTakeMachine() : StateMachine({.initial_state = PendingTakeModel{}}) {}

    std::vector<hegel::stateful::Rule<PendingTakeMachine>> rules() {
        using Rule = hegel::stateful::Rule<PendingTakeMachine>;

        return {
            Rule("press any key", [](hegel::TestCase& tc, auto& machine) {
                tc.assume(machine.recording());
                const int key = drawKey(tc);
                const int velocity = drawVelocity(tc, lowest_note_on_velocity);
                machine.press(key, velocity, drawGap(tc));
            }),
            Rule("press a held key", [](hegel::TestCase& tc, auto& machine) {
                tc.assume(machine.recording() && machine.state.held.any());
                const int key = machine.drawHeldKey(tc);
                const int velocity = drawVelocity(tc, lowest_note_on_velocity);
                machine.press(key, velocity, drawGap(tc));
            }),
            Rule("release a held key", [](hegel::TestCase& tc, auto& machine) {
                tc.assume(machine.recording() && machine.state.held.any());
                const int key = machine.drawHeldKey(tc);
                const int velocity = drawVelocity(tc, lowest_note_off_velocity);
                machine.release(key, velocity, drawGap(tc));
            }),
            Rule("release any key", [](hegel::TestCase& tc, auto& machine) {
                tc.assume(machine.recording());
                const int key = drawKey(tc);
                const int velocity = drawVelocity(tc, lowest_note_off_velocity);
                machine.release(key, velocity, drawGap(tc));
            }),
            Rule("fill up to a headroom", [](hegel::TestCase& tc, auto& machine) {
                tc.assume(machine.recording());
                const int key = drawKey(tc);
                const auto headroom = tc.draw(
                    "headroom",
                    gs::integers<std::size_t>({
                        .min_value = 0,
                        .max_value = largest_fill_headroom,
                    }),
                    repeatable
                );
                machine.fillUpTo(headroom, key);
            }),
            Rule("complete the take", [](hegel::TestCase& tc, auto& machine) {
                tc.assume(machine.recording());
                machine.complete(drawGap(tc));
            }),
            Rule("reset", [](hegel::TestCase&, auto& machine) {
                machine.reset();
            }),
        };
    }

    std::vector<hegel::stateful::Invariant<PendingTakeMachine>> invariants() {
        using Invariant = hegel::stateful::Invariant<PendingTakeMachine>;

        return {
            Invariant("recorded events match the model", [](const auto& machine) {
                if (!machine.recordedEventsMatch()) {
                    throw std::runtime_error(
                        "recorded events disagree with the capacity model"
                    );
                }
            }),
            Invariant("reserved releases fit the bound", [](const auto& machine) {
                if (machine.subject_.events().size() + machine.state.held.count()
                    > maximum_event_count) {
                    throw std::runtime_error(
                        "events and reserved releases exceed the event bound"
                    );
                }
            }),
            Invariant(
                "completion releases each held note once",
                [](const auto& machine) {
                    if (!machine.completionReleasesMatch()) {
                        throw std::runtime_error(
                            "completion did not release each held note once"
                        );
                    }
                }
            ),
            Invariant(
                "content ends at the last release",
                [](const auto& machine) {
                    if (!machine.contentEndsAtLastRelease()) {
                        throw std::runtime_error(
                            "content duration does not end at the last release"
                        );
                    }
                }
            ),
        };
    }

private:
    static int drawKey(hegel::TestCase& tc) {
        return tc.draw(
            "key",
            gs::integers<int>({.min_value = 0, .max_value = midi_key_count - 1}),
            repeatable
        );
    }

    static int drawVelocity(hegel::TestCase& tc, int lowest) {
        return tc.draw(
            "velocity",
            gs::integers<int>({
                .min_value = lowest,
                .max_value = highest_velocity,
            }),
            repeatable
        );
    }

    static Milliseconds drawGap(hegel::TestCase& tc) {
        return Milliseconds(tc.draw(
            "gap",
            gs::integers<int>({.min_value = 0, .max_value = longest_gap_ms}),
            repeatable
        ));
    }

    int drawHeldKey(hegel::TestCase& tc) const {
        return tc.draw("key", gs::sampled_from(keysIn(state.held)), repeatable);
    }

    bool recording() const {
        return !state.completed_at;
    }

    RecordedLoopEvent event(RecordedNoteKind kind, int key, int velocity) const {
        return {
            .time_ms = static_cast<std::uint64_t>(state.now.count()),
            .key = key,
            .velocity = velocity,
            .kind = kind,
        };
    }

    void press(int key, int velocity, Milliseconds gap) {
        state.now += gap;
        subject_.record(
            RecordedNoteKind::NoteOn,
            MidiMessage{.key = key, .velocity = velocity},
            state.now
        );

        // A Note On is accepted only while the take still has room for it and
        // for one release of every note held after it.
        const auto held_key = static_cast<std::size_t>(key);
        const std::size_t held_after = state.held.count()
            + (state.held.test(held_key) ? 0 : 1);
        if (state.recorded.size() + 1 + held_after > maximum_event_count) {
            return;
        }
        state.recorded.push_back(event(RecordedNoteKind::NoteOn, key, velocity));
        state.held.set(held_key);
    }

    void release(int key, int velocity, Milliseconds gap) {
        state.now += gap;
        subject_.record(
            RecordedNoteKind::NoteOff,
            MidiMessage{.key = key, .velocity = velocity},
            state.now
        );

        // A Note Off is accepted only for a held key.
        const auto held_key = static_cast<std::size_t>(key);
        if (!state.held.test(held_key)) {
            return;
        }
        state.held.reset(held_key);
        state.recorded.push_back(event(RecordedNoteKind::NoteOff, key, velocity));
    }

    // Single presses cannot reach the event bound within one sequence, so a
    // fill records released notes until little room remains.
    void fillUpTo(std::size_t headroom, int key) {
        while (freeEventPositions() >= headroom + released_note_events) {
            press(key, fill_velocity, 0ms);
            release(key, fill_velocity, 0ms);
        }
    }

    std::size_t freeEventPositions() const {
        return maximum_event_count - state.recorded.size() - state.held.count();
    }

    void complete(Milliseconds gap) {
        state.now += gap;
        subject_.finish(state.now);
        state.completed_at = state.now;
        state.released_at_completion = std::exchange(state.held, {});
    }

    void reset() {
        subject_.reset();
        state = PendingTakeModel{};
    }

    // Before completion the take holds exactly the recorded events; after
    // it, they are followed by completion's releases.
    bool recordedEventsMatch() const {
        const auto& events = subject_.events();
        const bool size_matches = recording()
            ? events.size() == state.recorded.size()
            : events.size() >= state.recorded.size();
        return size_matches
            && std::equal(
                state.recorded.begin(),
                state.recorded.end(),
                events.begin(),
                sameEvent
            );
    }

    // Completion appends one Note Off at the completion offset for each key
    // still held, in any order, and nothing else.
    bool completionReleasesMatch() const {
        const auto& events = subject_.events();
        if (!state.completed_at || events.size() < state.recorded.size()) {
            return true;
        }

        const auto completion_ms =
            static_cast<std::uint64_t>(state.completed_at->count());
        KeySet released;
        const auto first_release = state.recorded.size();
        for (auto index = first_release; index < events.size(); ++index) {
            const auto& synthesized = events[index];
            const auto key = static_cast<std::size_t>(synthesized.key);
            if (synthesized.kind != RecordedNoteKind::NoteOff
                || synthesized.time_ms != completion_ms
                || released.test(key)) {
                return false;
            }
            released.set(key);
        }
        return released == state.released_at_completion;
    }

    bool contentEndsAtLastRelease() const {
        if (!state.completed_at) {
            return true;
        }

        const auto& events = subject_.events();
        const auto last_release = std::find_if(
            events.rbegin(),
            events.rend(),
            [](const RecordedLoopEvent& recorded) {
                return recorded.kind == RecordedNoteKind::NoteOff;
            }
        );
        const auto expected = last_release == events.rend()
            ? Milliseconds::zero()
            : Milliseconds(static_cast<Milliseconds::rep>(last_release->time_ms));
        return subject_.contentDuration() == expected;
    }

    PendingTake subject_;
};

TEST(PendingTakePropertyTest, CommandsMatchIndependentModel) {
    hegel::Settings settings;
    settings.stateful_step_count = take_command_steps;

    hegel::test([](hegel::TestCase& tc) {
        PendingTakeMachine machine;
        hegel::stateful::run(machine, tc);
    }, settings);
}

} // namespace
