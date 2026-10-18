#include "audio_performance.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace audio_performance {
namespace {

// 120 beats per minute in 4/4: a bar lasts 2 s.
constexpr double beat_ms = 500.0;
constexpr int beats_per_bar = 4;
// A detached note ends this long before its written length.
constexpr double detached_gap_ms = 50.0;
// Zeta truncates take offsets and the guide period to whole milliseconds
// (loop_timing.cpp:7-10). A take's first note leaves this much before its
// whole-millisecond time, so every later event of the take is a whole number
// of milliseconds plus this much after it and truncates the same way in
// every run.
constexpr double take_start_lead_ms = 0.5;
// Controls of one slot selection: loop-slot control, then the slot key.
constexpr double selection_step_ms = 50.0;

constexpr int melody_velocity = 90;
constexpr int guide_velocity = 100;
constexpr int arpeggio_velocity = 80;

// MIDI keys.
constexpr int c2 = 36;
constexpr int g2 = 43;
constexpr int c3 = 48;
constexpr int d3 = 50;
constexpr int e3 = 52;
constexpr int g3 = 55;
constexpr int a3 = 57;
constexpr int c5 = 72;
constexpr int d5 = 74;
constexpr int e5 = 76;
constexpr int f5 = 77;
constexpr int g5 = 79;

// Bars and beats count from 1; a beat may be fractional.
constexpr double at(int bar, double beat) {
    return ((bar - 1) * beats_per_bar + (beat - 1.0)) * beat_ms;
}

struct Note {
    double beat;
    double beats;
    int key;
    bool legato{};
};

using Bar = std::vector<Note>;

struct TakeNote {
    double on_ms;
    int key;
    double beats;
    bool legato{};
};

double offTime(double on_ms, double beats, bool legato) {
    return on_ms + beats * beat_ms - (legato ? 0.0 : detached_gap_ms);
}

class Script {
public:
    void add(double at_ms, ActionKind kind, int key = 0, int value = 0) {
        steps_.push_back({.at_ms = at_ms, .kind = kind, .key = key, .value = value});
    }

    // Plays live bars from first_bar.
    void play(int first_bar, const std::vector<Bar>& bars, int velocity) {
        for (std::size_t index = 0; index < bars.size(); ++index) {
            const int bar = first_bar + static_cast<int>(index);
            for (const auto& note : bars[index]) {
                const double on = at(bar, note.beat);
                add(on, ActionKind::NoteOn, note.key, velocity);
                add(offTime(on, note.beats, note.legato), ActionKind::NoteOff, note.key);
            }
        }
    }

    // Records a take; held_key, if not zero, is pressed at held_on_ms and
    // still held when the take completes.
    void take(const std::vector<TakeNote>& notes, int held_key, double held_on_ms, int velocity) {
        for (std::size_t index = 0; index < notes.size(); ++index) {
            const auto& note = notes[index];
            const double on = index == 0 ? note.on_ms - take_start_lead_ms : note.on_ms;
            add(on, ActionKind::NoteOn, note.key, velocity);
            add(offTime(note.on_ms, note.beats, note.legato), ActionKind::NoteOff, note.key);
        }
        if (held_key != 0) {
            add(held_on_ms, ActionKind::NoteOn, held_key, velocity);
        }
    }

    // Records a guide take and completes it at completion_ms.
    void guideTake(const std::vector<TakeNote>& notes, int held_key, double held_on_ms, double completion_ms) {
        take(notes, held_key, held_on_ms, guide_velocity);
        add(completion_ms, ActionKind::LoopSlotControl);
        guide_take_times_.emplace_back(notes.front().on_ms - take_start_lead_ms, completion_ms);
    }

    void selectSlot(double at_ms, int slot_key) {
        add(at_ms, ActionKind::LoopSlotControl);
        add(at_ms + selection_step_ms, ActionKind::NoteOn, slot_key, guide_velocity);
        add(at_ms + 2 * selection_step_ms, ActionKind::NoteOff, slot_key);
    }

    Performance finish() && {
        std::stable_sort(steps_.begin(), steps_.end(), [](const Step& a, const Step& b) {
            return a.at_ms < b.at_ms;
        });
        Performance result{.steps = std::move(steps_), .guide_takes = {}};
        const auto index = [&result](double at_ms, ActionKind kind) {
            const auto found = std::find_if(result.steps.begin(), result.steps.end(), [&](const Step& step) {
                return step.at_ms == at_ms && step.kind == kind;
            });
            if (found == result.steps.end()) {
                throw std::logic_error("guide take step not found");
            }
            return static_cast<std::size_t>(found - result.steps.begin());
        };
        for (const auto& [first_note_ms, completion_ms] : guide_take_times_) {
            result.guide_takes.push_back({
                .first_note = index(first_note_ms, ActionKind::NoteOn),
                .completion = index(completion_ms, ActionKind::LoopSlotControl),
            });
        }
        return result;
    }

private:
    std::vector<Step> steps_;
    std::vector<std::pair<double, double>> guide_take_times_;
};

// "Ode to Joy" (public domain) in C major from C5, as four-bar phrases.
const std::vector<Bar> phrase_a = {
    {{1, 1, e5}, {2, 1, e5}, {3, 1, f5}, {4, 1, g5}},
    {{1, 1, g5}, {2, 1, f5}, {3, 1, e5}, {4, 1, d5}},
    {{1, 1, c5}, {2, 1, c5}, {3, 1, d5}, {4, 1, e5}},
    {{1, 1.5, e5, true}, {2.5, 0.5, d5}, {3, 1, d5}},
};
const std::vector<Bar> phrase_a_closing = {
    phrase_a[0],
    phrase_a[1],
    phrase_a[2],
    {{1, 1.5, d5, true}, {2.5, 0.5, c5}, {3, 1, c5}},
};
const std::vector<Bar> phrase_b_and_final_a = {
    {{1, 1, d5}, {2, 1, d5}, {3, 1, e5}, {4, 1, c5}},
    {{1, 1, d5}, {2, 0.5, e5, true}, {2.5, 0.5, f5}, {3, 1, e5}, {4, 1, c5}},
    {{1, 1, d5}, {2, 0.5, e5, true}, {2.5, 0.5, f5}, {3, 1, e5}, {4, 1, d5}},
    {{1, 1, c5}, {2, 1, d5}, {3, 2, g5}},
    phrase_a[0],
    phrase_a[1],
    phrase_a[2],
    {{1, 1.5, d5, true}, {2.5, 0.5, c5}, {3, 2, c5}},
};

} // namespace

Performance performance() {
    Script script;

    // 0:00 Live melody alone.
    script.play(1, phrase_a, melody_velocity);

    // 0:08 Record the guide: one bar of C2 G2 C3 and a rest. It loops from
    // 0:10 and plays alone until 0:12.
    script.selectSlot(7600.0, guide_slot_key);
    script.guideTake({{at(5, 1), c2, 1}, {at(5, 2), g2, 1}, {at(5, 3), c3, 1}}, 0, 0.0, at(6, 1));

    // 0:12 Melody over the guide.
    script.play(7, phrase_a_closing, melody_velocity);

    // 0:20 Next SoundFont and octave up; record the regular slot: two bars
    // of legato eighths from a quarter into the guide cycle, the last note
    // held at completion. Completion comes 10 ms before the loop point, so
    // no note of the first cycle is skipped.
    script.add(20100.0, ActionKind::NextSoundFont);
    script.add(20150.0, ActionKind::OctaveUp);
    script.selectSlot(20200.0, regular_slot_key);
    const std::vector<int> arpeggio = {c3, e3, g3, a3, g3, e3, c3, e3, g3, a3, g3, e3, c3, e3, g3};
    std::vector<TakeNote> arpeggio_notes;
    for (std::size_t index = 0; index < arpeggio.size(); ++index) {
        const double beat = 2.0 + 0.5 * static_cast<double>(index);
        arpeggio_notes.push_back({at(11, beat), arpeggio[index], 0.5, true});
    }
    script.take(arpeggio_notes, a3, at(13, 1.5), arpeggio_velocity);
    script.add(at(13, 2) - 10.0, ActionKind::LoopSlotControl);
    script.add(at(13, 2) + 50.0, ActionKind::NoteOff, a3);
    // The live route adopted the regular slot's selection: back to the
    // first SoundFont and octave for the melody.
    script.add(24700.0, ActionKind::NextSoundFont);
    script.add(24750.0, ActionKind::OctaveDown);

    // 0:26 Melody over both loops.
    script.play(14, phrase_b_and_final_a, melody_velocity);

    // 0:42 Replace the regular take: stop it, arm it again an octave up and
    // record one bar with its last note held at completion.
    script.selectSlot(42100.0, regular_slot_key);
    script.selectSlot(42300.0, regular_slot_key);
    script.add(42450.0, ActionKind::OctaveUp);
    script.take({{at(22, 3), e3, 1}, {at(22, 4), d3, 0.5, true}, {at(22, 4.5), c3, 0.5, true}, {at(23, 1), g3, 1}},
                c3, at(23, 2), arpeggio_velocity);
    script.add(at(23, 3) - 10.0, ActionKind::LoopSlotControl);
    script.add(at(23, 3) + 50.0, ActionKind::NoteOff, c3);

    // 0:50 Stop the guide, which stops the regular slot too; Next SoundFont
    // and octave down; record a new half-bar guide with its last note held
    // at completion.
    script.selectSlot(50100.0, guide_slot_key);
    script.add(50300.0, ActionKind::NextSoundFont);
    script.add(50350.0, ActionKind::OctaveDown);
    script.selectSlot(50500.0, guide_slot_key);
    script.guideTake({{at(26, 3), c2, 1}}, g2, at(26, 4), at(27, 1));
    script.add(at(27, 1) + 50.0, ActionKind::NoteOff, g2);

    // 0:58 Shutdown while the new guide loops.
    script.add(at(30, 1) + 250.0, ActionKind::Shutdown);

    return std::move(script).finish();
}

} // namespace audio_performance
