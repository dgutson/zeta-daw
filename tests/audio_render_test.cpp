#include "audio_analysis.hpp"
#include "audio_performance.hpp"
#include "fake_midi_input.hpp"
#include "../application.hpp"
#include "../configuration.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using audio_performance::ActionKind;
using audio_performance::Performance;
using Clock = std::chrono::steady_clock;

// MIDI Machine Control commands bound to the controls.
constexpr int mmc_next_soundfont = 0x01;
constexpr int mmc_octave_down = 0x02;
constexpr int mmc_loop_slot = 0x05;
constexpr int mmc_octave_up = 0x06;

constexpr int note_on = 0x90;
constexpr int note_off = 0x80;
// The peak stays near -10 dBFS.
constexpr double gain = 0.15;
// Time from starting the application to the performance's first step.
constexpr auto lead_in = std::chrono::milliseconds(300);
// A thread woken from sleep on a busy machine can be several milliseconds
// late, so the harness sleeps until this long before each step and spins for
// the rest.
constexpr auto spin_before_step = std::chrono::milliseconds(5);
// Zeta truncates the guide period to whole milliseconds, and each guide
// take's first note is scheduled 0.5 ms before a whole millisecond. The
// period therefore truncates as in the reference only while the first note
// and the completion leave less than this far apart in lateness.
constexpr double take_lateness_limit_ms = 0.4;
constexpr std::size_t max_attempts = 3;
// The first guide: completed 10 s after the first note, 20 repetitions of 2 s.
constexpr audio_analysis::Guide guide{.first_repetition_ms = 10000.0, .period_ms = 2000.0, .repetitions = 20};

const std::filesystem::path render_directory = ZETA_AUDIO_RENDER_DIR;
const std::filesystem::path reference_path = ZETA_AUDIO_REFERENCE;
// FluidSynth's file driver writes this file into the working directory.
const std::filesystem::path capture_path = "fluidsynth.wav";
const std::filesystem::path window_csv_path = "windows.csv";

// How late a step left its scheduled time, and how long its call took.
struct StepTiming {
    double late_ms{};
    double call_ms{};
};

double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void waitUntil(Clock::time_point target) {
    std::this_thread::sleep_until(target - spin_before_step);
    while (Clock::now() < target) {
    }
}

bool recording() {
    const char* value = std::getenv("ZETA_RECORD_AUDIO_REFERENCE");
    return value != nullptr && std::string_view(value) == "1";
}

zeta::MidiControlBinding machineControl(int command) {
    return {.type = zeta::MidiControlType::MachineControl, .number = command};
}

zeta::ApplicationConfig renderConfig() {
    const std::filesystem::path soundfont = ZETA_AUDIO_SOUNDFONT;
    zeta::ApplicationConfig config{
        .audio = {},
        .audio_input = std::nullopt,
        .loop_slots = {{.key = audio_performance::guide_slot_key}, {.key = audio_performance::regular_slot_key}},
        .soundfonts = {
            // FM Electric Bass and Oink Grind.
            {.id = "first", .file = soundfont, .bank = 0, .preset = 19, .key = std::nullopt},
            {.id = "second", .file = soundfont, .bank = 0, .preset = 46, .key = std::nullopt},
        },
        .midi_control_change_mappings = {},
        .loop_slot_by_note_control = machineControl(mmc_loop_slot),
        .next_soundfont_control = machineControl(mmc_next_soundfont),
        .soundfont_by_note_control = std::nullopt,
        .octave_down_control = machineControl(mmc_octave_down),
        .octave_up_control = machineControl(mmc_octave_up),
    };
    config.audio.driver = "file";
    config.audio.gain = gain;
    return config;
}

void emitMachineControl(int command) {
    fake_midi_input::emitMidi({
        .type = static_cast<int>(zeta::MidiMessageType::MachineControl),
        .machine_control_command = command,
    });
}

void send(const audio_performance::Step& step, zeta::Application& application) {
    switch (step.kind) {
    case ActionKind::NoteOn:
        fake_midi_input::emitMidi({.type = note_on, .key = step.key, .velocity = step.value});
        break;
    case ActionKind::NoteOff:
        fake_midi_input::emitMidi({.type = note_off, .key = step.key});
        break;
    case ActionKind::LoopSlotControl:
        emitMachineControl(mmc_loop_slot);
        break;
    case ActionKind::NextSoundFont:
        emitMachineControl(mmc_next_soundfont);
        break;
    case ActionKind::OctaveDown:
        emitMachineControl(mmc_octave_down);
        break;
    case ActionKind::OctaveUp:
        emitMachineControl(mmc_octave_up);
        break;
    case ActionKind::Shutdown:
        application.shutdownRequested();
        break;
    }
}

std::string_view kindName(ActionKind kind) {
    switch (kind) {
    case ActionKind::NoteOn:
        return "Note On";
    case ActionKind::NoteOff:
        return "Note Off";
    case ActionKind::LoopSlotControl:
        return "loop slot";
    case ActionKind::NextSoundFont:
        return "next SoundFont";
    case ActionKind::OctaveDown:
        return "octave down";
    case ActionKind::OctaveUp:
        return "octave up";
    case ActionKind::Shutdown:
        return "shutdown";
    }
    return "?";
}

// Plays the performance once through the real Application and FluidSynth's
// file driver, which writes capture_path.
std::vector<StepTiming> play(const Performance& performance) {
    auto application = std::make_unique<zeta::Application>(renderConfig(), fake_midi_input::makeInput());
    std::thread runner([&application] { application->run(); });
    const auto origin = Clock::now() + lead_in;
    const auto scheduled = [origin](double at_ms) {
        return origin + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(at_ms));
    };
    std::vector<StepTiming> timings;
    timings.reserve(performance.steps.size());
    for (const auto& step : performance.steps) {
        waitUntil(scheduled(step.at_ms));
        const auto left = Clock::now();
        send(step, *application);
        const auto done = Clock::now();
        timings.push_back({.late_ms = milliseconds(left - scheduled(step.at_ms)), .call_ms = milliseconds(done - left)});
    }
    runner.join();
    waitUntil(scheduled(performance.steps.back().at_ms + audio_performance::silence_after_shutdown_ms));
    application.reset();
    return timings;
}

double takeLatenessDifference(const audio_performance::GuideTake& take, const std::vector<StepTiming>& timings) {
    return timings[take.completion].late_ms - timings[take.first_note].late_ms;
}

bool timingAccepted(const Performance& performance, const std::vector<StepTiming>& timings) {
    return std::ranges::all_of(performance.guide_takes, [&timings](const auto& take) {
        return std::abs(takeLatenessDifference(take, timings)) < take_lateness_limit_ms;
    });
}

std::string harnessReport(const Performance& performance, const std::vector<std::vector<StepTiming>>& attempts) {
    std::string report = std::format(
        "In each of {} attempts the harness sent a guide take's first note and its completion {} ms or more apart "
        "in lateness, so the guide period may differ from the reference's. The machine was probably busy.\n",
        attempts.size(),
        take_lateness_limit_ms
    );
    for (std::size_t attempt = 0; attempt < attempts.size(); ++attempt) {
        for (const auto& take : performance.guide_takes) {
            report += std::format(
                "attempt {}: first note at {:.1f} ms left {:.3f} ms late, completion at {:.1f} ms left {:.3f} ms late\n",
                attempt + 1,
                performance.steps[take.first_note].at_ms,
                attempts[attempt][take.first_note].late_ms,
                performance.steps[take.completion].at_ms,
                attempts[attempt][take.completion].late_ms
            );
        }
    }
    report += "Each step: time, kind and key, then lateness and call time in ms for each attempt.\n";
    for (std::size_t index = 0; index < performance.steps.size(); ++index) {
        const auto& step = performance.steps[index];
        report += std::format("{:9.1f} {:<14} {:3}", step.at_ms, kindName(step.kind), step.key);
        for (const auto& timings : attempts) {
            report += std::format(" | {:7.3f} {:7.3f}", timings[index].late_ms, timings[index].call_ms);
        }
        report += '\n';
    }
    return report;
}

// Values the test reports through GoogleTest, which --gtest_output=json
// writes out.
void recordValue(const std::string& key, double value) {
    testing::Test::RecordProperty(key, std::format("{:.3f}", value));
}

void recordValue(const std::string& key, const std::optional<double>& value) {
    if (value) {
        recordValue(key, *value);
    }
}

void recordTiming(const Performance& performance, const std::vector<StepTiming>& timings) {
    double largest_lateness = 0.0;
    for (const auto& timing : timings) {
        largest_lateness = std::max(largest_lateness, timing.late_ms);
    }
    double largest_difference = 0.0;
    for (const auto& take : performance.guide_takes) {
        largest_difference = std::max(largest_difference, std::abs(takeLatenessDifference(take, timings)));
    }
    recordValue("largest_lateness_ms", largest_lateness);
    recordValue("largest_take_lateness_difference_ms", largest_difference);
}

void recordComparison(const audio_analysis::Comparison& comparison) {
    recordValue("worst_quiet_db", comparison.worst_quiet_db);
    recordValue("worst_loud_db", comparison.worst_loud_db);
    recordValue("worst_click_db", comparison.worst_click_db);
    recordValue("worst_peak_quiet_db", comparison.worst_peak_quiet_db);
    recordValue("worst_peak_loud_db", comparison.worst_peak_loud_db);
    recordValue("reference_period_ms", comparison.reference_period_ms);
    recordValue("capture_period_ms", comparison.capture_period_ms);
    if (comparison.reference_period_ms && comparison.capture_period_ms) {
        recordValue("period_difference_ms", *comparison.capture_period_ms - *comparison.reference_period_ms);
    }
    recordValue("reference_peak_dbfs", comparison.reference_peak_dbfs);
    recordValue("capture_peak_dbfs", comparison.capture_peak_dbfs);
}

void recordTolerances() {
    recordValue("tau_ms", audio_analysis::tau_ms);
    recordValue("level_delta_db", audio_analysis::level_delta_db);
    recordValue("peak_delta_db", audio_analysis::peak_delta_db);
    recordValue("drift_limit_ms", audio_analysis::drift_limit_ms);
    recordValue("take_lateness_limit_ms", take_lateness_limit_ms);
}

std::string failureReport(const audio_analysis::Comparison& comparison) {
    std::string report;
    for (const auto& failure : comparison.failures) {
        report += audio_analysis::describe(failure) + '\n';
    }
    const auto period = [](const std::optional<double>& value) {
        return value ? std::format("{:.3f} ms", *value) : std::string("not measured");
    };
    report += std::format(
        "Guide period: reference {}, capture {}.\n",
        period(comparison.reference_period_ms),
        period(comparison.capture_period_ms)
    );
    return report;
}

} // namespace

// Plays the performance and compares the capture with the reference, or with
// ZETA_RECORD_AUDIO_REFERENCE=1 writes the capture as the new reference.
TEST(AudioRenderTest, PerformanceMatchesReference) {
    std::filesystem::create_directories(render_directory);
    std::filesystem::current_path(render_directory);
    std::filesystem::remove(window_csv_path);
    recordTolerances();

    const auto performance = audio_performance::performance();
    std::vector<std::vector<StepTiming>> discarded;
    std::optional<std::vector<StepTiming>> accepted;
    while (!accepted && discarded.size() < max_attempts) {
        auto timings = play(performance);
        if (timingAccepted(performance, timings)) {
            accepted = std::move(timings);
        } else {
            discarded.push_back(std::move(timings));
        }
    }
    RecordProperty("discarded_attempts", static_cast<int>(discarded.size()));
    ASSERT_TRUE(accepted) << harnessReport(performance, discarded);
    recordTiming(performance, *accepted);

    const auto capture = audio_analysis::readAudio(capture_path);
    if (recording()) {
        // Compared with itself, the capture can fail only the checks a
        // reference must pass: no clipping, its peak level and its guide
        // period.
        const auto check = audio_analysis::compare(capture, capture, guide);
        recordValue("capture_peak_dbfs", check.capture_peak_dbfs);
        recordValue("capture_period_ms", check.capture_period_ms);
        ASSERT_TRUE(check.failures.empty()) << failureReport(check) << "The reference was not written.";
        std::filesystem::create_directories(reference_path.parent_path());
        audio_analysis::writeAudio(reference_path, capture);
        return;
    }

    const auto comparison = audio_analysis::compare(audio_analysis::readAudio(reference_path), capture, guide);
    recordComparison(comparison);
    if (!comparison.failures.empty()) {
        audio_analysis::writeWindowCsv(window_csv_path, comparison);
        FAIL() << failureReport(comparison) << "The capture and the per-window values are in "
               << render_directory / capture_path << " and " << render_directory / window_csv_path << '.';
    }
}
