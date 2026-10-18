#include "audio_analysis.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sndfile.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace aa = audio_analysis;

using aa::Audio;
using aa::Comparison;
using aa::Failure;
using aa::FailureKind;
using aa::Guide;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::StartsWith;

// A synthetic performance of sine tones, shaped like the rendered one: a
// lone melody note, a guide bass line below 130 Hz repeating under melody
// notes, and a lone note after it. Times are ms from the first note.
struct Note {
    double start_ms{};
    double end_ms{};
    double frequency_hz{};
    double amplitude{};
};

constexpr double lead_ms = 100.0;
constexpr double length_ms = 1800.0;
// Linear ramps, so that only the planted changes click.
constexpr double attack_ms = 5.0;
constexpr double release_ms = 20.0;
constexpr double melody_amplitude = 0.1;
constexpr double bass_amplitude = 0.12;
constexpr Guide guide{
    .first_repetition_ms = 250.0,
    .period_ms = 150.0,
    .repetitions = 6,
};
constexpr double held_start_ms = 500.0;
constexpr double held_end_ms = 1000.0;
constexpr double lone_start_ms = 1250.0;
constexpr double lone_end_ms = 1450.0;
// After the lone note, where the reference is silent.
constexpr double extra_start_ms = 1550.0;
constexpr double extra_end_ms = 1650.0;
constexpr double lone_hz = 880.0;
constexpr double semitone = 1.0594630943592953;
constexpr double bin_hz = static_cast<double>(aa::sample_rate) / aa::fft_size;
constexpr double half_frame_ms = 1000.0 * (aa::fft_size / 2) / aa::sample_rate;

struct Variation {
    // Each guide repetition starts this much later than the one before.
    double guide_drift_ms{};
    double lone_shift_ms{};
    double lone_hz{::lone_hz};
    bool lone_stuck{};
};

std::vector<Note> notes(const Variation& variation = {}) {
    std::vector<Note> score{
        {0.0, 200.0, 523.25, melody_amplitude},
        {300.0, 450.0, 659.26, melody_amplitude},
        {held_start_ms, held_end_ms, 783.99, melody_amplitude},
    };
    constexpr std::array<double, 4> bass_hz{55.0, 65.41, 82.41, 73.42};
    constexpr double bass_step_ms = 37.5;
    constexpr double bass_note_ms = 32.0;
    for (int repetition = 0; repetition < guide.repetitions; ++repetition) {
        const auto start = guide.first_repetition_ms
            + repetition * (guide.period_ms + variation.guide_drift_ms);
        for (std::size_t i = 0; i < bass_hz.size(); ++i) {
            const auto at = start + static_cast<double>(i) * bass_step_ms;
            score.push_back({at, at + bass_note_ms, bass_hz[i], bass_amplitude});
        }
    }
    const auto lone_start = lone_start_ms + variation.lone_shift_ms;
    score.push_back({
        lone_start,
        variation.lone_stuck ? length_ms : lone_end_ms,
        variation.lone_hz,
        melody_amplitude,
    });
    return score;
}

double envelope(double ms, double duration_ms) {
    const auto attack = std::min(1.0, ms / attack_ms);
    if (ms <= duration_ms) {
        return attack;
    }
    return attack * std::max(0.0, 1.0 - (ms - duration_ms) / release_ms);
}

double frames(double ms) {
    return ms * aa::sample_rate / 1000.0;
}

// Notes may start between samples, so that a drift of a fraction of a
// sample is rendered exactly.
std::vector<double> mix(const std::vector<Note>& score) {
    std::vector<double> samples(static_cast<std::size_t>(frames(lead_ms + length_ms)), 0.0);
    for (const auto& note : score) {
        const auto start = frames(lead_ms + note.start_ms);
        const auto stop = frames(lead_ms + note.end_ms + release_ms);
        const auto last = std::min(samples.size(), static_cast<std::size_t>(std::ceil(stop)));
        for (auto n = static_cast<std::size_t>(std::ceil(start)); n < last; ++n) {
            const auto ms = (static_cast<double>(n) - start) * 1000.0 / aa::sample_rate;
            samples[n] += note.amplitude * envelope(ms, note.end_ms - note.start_ms)
                * std::sin(2.0 * std::numbers::pi * note.frequency_hz * ms / 1000.0);
        }
    }
    return samples;
}

// Both channels carry the mix; values beyond full scale clip.
Audio toAudio(const std::vector<double>& samples) {
    Audio audio;
    for (const auto value : samples) {
        const auto sample = static_cast<std::int16_t>(std::clamp(std::lround(value * 32768.0), -32768L, 32767L));
        audio.samples.insert(audio.samples.end(), aa::channel_count, sample);
    }
    return audio;
}

Audio render(const Variation& variation = {}) {
    return toAudio(mix(notes(variation)));
}

// The analysis measures times from the first frame with a sample above
// -80 dBFS, which is 4 / 32768 or more.
std::size_t firstSound(const Audio& audio) {
    constexpr int smallest_sound = 4;
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        if (std::abs(static_cast<int>(audio.samples[i])) >= smallest_sound) {
            return i / aa::channel_count;
        }
    }
    return 0;
}

double msAfterFirstSound(const Audio& audio, std::size_t frame) {
    return (static_cast<double>(frame) - static_cast<double>(firstSound(audio))) * 1000.0 / aa::sample_rate;
}

// The first frame of the first window that starts at least ms after the
// first sound.
std::size_t windowFrameAfter(const Audio& audio, double ms) {
    const auto first_window = static_cast<double>(firstSound(audio)) - frames(aa::pad_ms);
    const auto target = static_cast<double>(firstSound(audio)) + frames(ms);
    const auto index = std::ceil((target - first_window) / aa::window_samples);
    return static_cast<std::size_t>(first_window + index * aa::window_samples);
}

std::size_t frameAfter(const Audio& audio, double ms) {
    return firstSound(audio) + static_cast<std::size_t>(std::lround(frames(ms)));
}

std::string describeAll(const Comparison& comparison) {
    std::string text;
    for (const auto& failure : comparison.failures) {
        text += aa::describe(failure) + "\n";
    }
    return text;
}

std::set<FailureKind> kinds(const Comparison& comparison) {
    std::set<FailureKind> result;
    for (const auto& failure : comparison.failures) {
        result.insert(failure.kind);
    }
    return result;
}

bool isSpectral(FailureKind kind) {
    return kind == FailureKind::PeakTooQuiet || kind == FailureKind::PeakTooLoud;
}

// Whether a failure of this kind starts within [from_ms, to_ms].
bool reported(const Comparison& comparison, FailureKind kind, double from_ms, double to_ms) {
    return std::any_of(comparison.failures.begin(), comparison.failures.end(), [&](const Failure& failure) {
        return failure.kind == kind && failure.at_ms >= from_ms && failure.at_ms <= to_ms;
    });
}

// Whether every failure overlaps [from_ms, to_ms], widened by its kind's
// resolution: τ and a window for levels and clicks, half an FFT frame and τ
// for spectral peaks.
bool allNear(const Comparison& comparison, double from_ms, double to_ms) {
    return std::all_of(comparison.failures.begin(), comparison.failures.end(), [&](const Failure& failure) {
        const auto margin = aa::tau_ms + (isSpectral(failure.kind) ? half_frame_ms : aa::window_ms);
        return failure.at_ms <= to_ms + margin && failure.at_ms + failure.length_ms >= from_ms - margin;
    });
}

TEST(AudioAnalysisTest, UnmodifiedPerformancePasses) {
    const auto reference = render();

    const auto comparison = aa::compare(reference, reference, guide);

    EXPECT_THAT(comparison.failures, IsEmpty()) << describeAll(comparison);
    ASSERT_TRUE(comparison.reference_period_ms);
    EXPECT_NEAR(*comparison.reference_period_ms, guide.period_ms, aa::drift_limit_ms);
    EXPECT_EQ(comparison.capture_period_ms, comparison.reference_period_ms);
    for (const auto& worst : {
             comparison.worst_quiet_db,
             comparison.worst_loud_db,
             comparison.worst_click_db,
             comparison.worst_peak_quiet_db,
             comparison.worst_peak_loud_db,
         }) {
        ASSERT_TRUE(worst);
        EXPECT_LT(*worst, 0.0);
    }
}

TEST(AudioAnalysisTest, TimingShiftsUpToHalfOfTauPass) {
    const auto reference = render();

    const auto later = aa::compare(reference, render({.lone_shift_ms = aa::tau_ms / 2}), guide);
    const auto earlier = aa::compare(reference, render({.lone_shift_ms = -aa::tau_ms / 2}), guide);

    EXPECT_THAT(later.failures, IsEmpty()) << describeAll(later);
    EXPECT_THAT(earlier.failures, IsEmpty()) << describeAll(earlier);
}

TEST(AudioAnalysisTest, DriftOfHalfTheLimitPasses) {
    const auto comparison = aa::compare(render(), render({.guide_drift_ms = aa::drift_limit_ms / 2}), guide);

    EXPECT_THAT(comparison.failures, IsEmpty()) << describeAll(comparison);
}

TEST(AudioAnalysisTest, ReportsADropoutCoveringAWindowAsTooQuiet) {
    // A gap must cover nine tenths of a window to lower its level by
    // level_delta_db; this 5 ms gap covers one window exactly.
    const auto reference = render();
    auto capture = reference;
    const auto first = windowFrameAfter(capture, 700.0);
    std::fill_n(
        capture.samples.begin() + static_cast<std::ptrdiff_t>(first * aa::channel_count),
        aa::window_samples * aa::channel_count,
        std::int16_t{0}
    );
    const auto at_ms = msAfterFirstSound(capture, first);

    const auto comparison = aa::compare(reference, capture, guide);

    EXPECT_TRUE(reported(comparison, FailureKind::TooQuiet, at_ms - aa::window_ms, at_ms + aa::window_ms))
        << describeAll(comparison);
    EXPECT_TRUE(allNear(comparison, at_ms, at_ms + aa::window_ms)) << describeAll(comparison);
    const auto quiet = std::find_if(comparison.failures.begin(), comparison.failures.end(), [](const Failure& failure) {
        return failure.kind == FailureKind::TooQuiet;
    });
    ASSERT_NE(quiet, comparison.failures.end());
    EXPECT_THAT(aa::describe(*quiet), StartsWith("too quiet at 0.70"));
    EXPECT_THAT(aa::describe(*quiet), HasSubstr(" for 5.0 ms: -240.0 dBFS, lowest allowed "));
}

TEST(AudioAnalysisTest, ReportsADropoutAcrossTwoWindowsAsClicks) {
    // Half of each window is silent, 3 dB down; the cut edges click.
    const auto reference = render();
    auto capture = reference;
    const auto first = windowFrameAfter(capture, 700.0) + aa::window_samples / 2;
    std::fill_n(
        capture.samples.begin() + static_cast<std::ptrdiff_t>(first * aa::channel_count),
        aa::window_samples * aa::channel_count,
        std::int16_t{0}
    );
    const auto at_ms = msAfterFirstSound(capture, first);

    const auto comparison = aa::compare(reference, capture, guide);

    EXPECT_TRUE(reported(comparison, FailureKind::Click, at_ms - aa::window_ms, at_ms + aa::window_ms))
        << describeAll(comparison);
    EXPECT_TRUE(allNear(comparison, at_ms, at_ms + aa::window_ms)) << describeAll(comparison);
}

TEST(AudioAnalysisTest, ReportsAnOnsetTwoTauLateAsTooQuiet) {
    const auto comparison = aa::compare(render(), render({.lone_shift_ms = 2 * aa::tau_ms}), guide);

    EXPECT_TRUE(reported(comparison, FailureKind::TooQuiet, lone_start_ms, lone_start_ms + 2 * aa::tau_ms))
        << describeAll(comparison);
    EXPECT_TRUE(allNear(comparison, lone_start_ms, lone_start_ms + 2 * aa::tau_ms)) << describeAll(comparison);
}

TEST(AudioAnalysisTest, ReportsAnOnsetTwoTauEarlyAsTooLoud) {
    const auto comparison = aa::compare(render(), render({.lone_shift_ms = -2 * aa::tau_ms}), guide);

    EXPECT_TRUE(reported(
        comparison,
        FailureKind::TooLoud,
        lone_start_ms - 2 * aa::tau_ms - aa::window_ms,
        lone_start_ms
    )) << describeAll(comparison);
    EXPECT_TRUE(allNear(comparison, lone_start_ms - 2 * aa::tau_ms, lone_start_ms)) << describeAll(comparison);
}

TEST(AudioAnalysisTest, ReportsAOneSampleStepAsAClick) {
    const auto reference = render();
    auto capture = reference;
    const auto frame = frameAfter(capture, 750.0);
    constexpr auto step = static_cast<std::int16_t>(0.05 * 32768);
    for (int channel = 0; channel < aa::channel_count; ++channel) {
        capture.samples[frame * aa::channel_count + static_cast<std::size_t>(channel)] += step;
    }
    const auto at_ms = msAfterFirstSound(capture, frame);

    const auto comparison = aa::compare(reference, capture, guide);

    EXPECT_THAT(kinds(comparison), ElementsAre(FailureKind::Click)) << describeAll(comparison);
    EXPECT_TRUE(reported(comparison, FailureKind::Click, at_ms - aa::window_ms, at_ms + aa::window_ms))
        << describeAll(comparison);
}

TEST(AudioAnalysisTest, ReportsAClippedStretch) {
    constexpr double from_ms = 800.0;
    constexpr double to_ms = 850.0;
    const auto reference = render();
    auto samples = mix(notes());
    const auto first = static_cast<std::size_t>(frames(lead_ms + from_ms));
    const auto last = static_cast<std::size_t>(frames(lead_ms + to_ms));
    for (auto n = first; n < last; ++n) {
        samples[n] *= 8.0;
    }
    const auto capture = toAudio(samples);
    const auto at_ms = msAfterFirstSound(capture, first);

    const auto comparison = aa::compare(reference, capture, guide);

    EXPECT_TRUE(reported(comparison, FailureKind::Clipped, at_ms - aa::window_ms, at_ms + aa::window_ms))
        << describeAll(comparison);
    EXPECT_TRUE(allNear(comparison, at_ms, at_ms + to_ms - from_ms)) << describeAll(comparison);
}

TEST(AudioAnalysisTest, ReportsAnExtraNoteAsTooLoud) {
    auto score = notes();
    score.push_back({extra_start_ms, extra_end_ms, 1046.5, melody_amplitude});

    const auto comparison = aa::compare(render(), toAudio(mix(score)), guide);

    EXPECT_TRUE(reported(
        comparison,
        FailureKind::TooLoud,
        extra_start_ms - aa::window_ms,
        extra_start_ms + aa::window_ms
    )) << describeAll(comparison);
    EXPECT_TRUE(reported(comparison, FailureKind::PeakTooLoud, extra_start_ms - half_frame_ms, extra_end_ms))
        << describeAll(comparison);
    EXPECT_TRUE(allNear(comparison, extra_start_ms, extra_end_ms + release_ms)) << describeAll(comparison);
}

TEST(AudioAnalysisTest, ReportsAStuckNoteAsTooLoud) {
    const auto comparison = aa::compare(render(), render({.lone_stuck = true}), guide);

    EXPECT_TRUE(reported(
        comparison,
        FailureKind::TooLoud,
        lone_end_ms,
        lone_end_ms + release_ms + aa::tau_ms + aa::window_ms
    )) << describeAll(comparison);
    EXPECT_TRUE(allNear(comparison, lone_end_ms, length_ms)) << describeAll(comparison);
}

TEST(AudioAnalysisTest, ReportsDriftOfTwiceTheLimitOnlyThroughTheGuidePeriod) {
    // The last repetition is 0.5 ms late, well within τ.
    const auto comparison = aa::compare(render(), render({.guide_drift_ms = 2 * aa::drift_limit_ms}), guide);

    ASSERT_THAT(kinds(comparison), ElementsAre(FailureKind::Drift)) << describeAll(comparison);
    EXPECT_NEAR(comparison.failures.front().measured, 2 * aa::drift_limit_ms, aa::drift_limit_ms / 2);
    EXPECT_EQ(comparison.failures.front().at_ms, guide.first_repetition_ms);
}

TEST(AudioAnalysisTest, ReportsAToneMovedASemitoneUpAsSpectralPeaks) {
    const auto comparison = aa::compare(render(), render({.lone_hz = lone_hz * semitone}), guide);

    ASSERT_THAT(kinds(comparison), ElementsAre(FailureKind::PeakTooQuiet, FailureKind::PeakTooLoud))
        << describeAll(comparison);
    for (const auto& failure : comparison.failures) {
        const auto expected_hz = failure.kind == FailureKind::PeakTooQuiet ? lone_hz : lone_hz * semitone;
        EXPECT_NEAR(failure.frequency_hz, expected_hz, bin_hz) << aa::describe(failure);
    }
    EXPECT_TRUE(allNear(comparison, lone_start_ms, lone_end_ms)) << describeAll(comparison);
}

TEST(AudioAnalysisTest, ReportsAReferenceThatPeaksAboveTheLimit) {
    auto score = notes();
    score.push_back({extra_start_ms, extra_end_ms, 1046.5, 0.6});
    const auto reference = toAudio(mix(score));

    const auto comparison = aa::compare(reference, reference, guide);

    EXPECT_THAT(kinds(comparison), ElementsAre(FailureKind::ReferenceTooLoud)) << describeAll(comparison);
    EXPECT_GT(comparison.reference_peak_dbfs, aa::reference_peak_limit_dbfs);
}

TEST(AudioAnalysisTest, ReportsAReferenceWhoseGuidePeriodDiffersFromTheScript) {
    const auto reference = render({.guide_drift_ms = 2 * aa::drift_limit_ms});

    const auto comparison = aa::compare(reference, reference, guide);

    EXPECT_THAT(kinds(comparison), ElementsAre(FailureKind::ReferencePeriod)) << describeAll(comparison);
}

TEST(AudioAnalysisTest, ReportsACaptureThatEndsBeforeTheGuidesLastRepetition) {
    const auto reference = render();
    auto capture = reference;
    capture.samples.resize(frameAfter(capture, 700.0) * aa::channel_count);

    const auto comparison = aa::compare(reference, capture, guide);

    EXPECT_FALSE(comparison.capture_period_ms);
    EXPECT_TRUE(std::any_of(comparison.failures.begin(), comparison.failures.end(), [](const Failure& failure) {
        return failure.kind == FailureKind::Drift && std::isnan(failure.measured);
    })) << describeAll(comparison);
    EXPECT_TRUE(reported(comparison, FailureKind::TooQuiet, 700.0 - aa::window_ms, 700.0 + aa::tau_ms))
        << describeAll(comparison);
}

std::filesystem::path testFile(const std::string& name) {
    const auto* const test = ::testing::UnitTest::GetInstance()->current_test_info();
    return std::filesystem::path(::testing::TempDir()) / (std::string(test->name()) + "_" + name);
}

TEST(AudioAnalysisTest, ReadsBackTheSamplesItWritesAsWavAndFlac) {
    const auto audio = render();

    for (const auto* const name : {"audio.wav", "audio.flac"}) {
        const auto path = testFile(name);
        aa::writeAudio(path, audio);
        EXPECT_EQ(aa::readAudio(path).samples, audio.samples) << name;
        std::filesystem::remove(path);
    }
}

TEST(AudioAnalysisTest, RejectsMissingAndNonStereoFiles) {
    const auto mono = testFile("mono.wav");
    {
        SndfileHandle file(mono.string(), SFM_WRITE, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 1, aa::sample_rate);
        const std::array<std::int16_t, 4> samples{};
        ASSERT_EQ(file.writef(samples.data(), samples.size()), static_cast<sf_count_t>(samples.size()));
    }

    EXPECT_THROW(aa::readAudio(mono), std::runtime_error);
    EXPECT_THROW(aa::readAudio(testFile("missing.wav")), std::runtime_error);
    std::filesystem::remove(mono);
}

TEST(AudioAnalysisTest, WritesOneCsvRowPerWindow) {
    const auto reference = render();
    const auto comparison = aa::compare(reference, reference, guide);
    const auto path = testFile("windows.csv");

    aa::writeWindowCsv(path, comparison);

    std::ifstream file(path);
    std::string header;
    std::getline(file, header);
    EXPECT_THAT(header, HasSubstr("time_ms,reference_db,capture_db,"));
    std::size_t rows = 0;
    std::string row;
    while (std::getline(file, row)) {
        ++rows;
    }
    EXPECT_EQ(rows, comparison.windows.size());
    std::filesystem::remove(path);
}

} // namespace
