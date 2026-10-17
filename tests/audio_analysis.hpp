#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Reads and writes the audio test's recordings and compares a capture with
// the reference recording: levels and clicks in 5 ms windows, clipping,
// spectral peaks, and the guide's repetition period. All times are measured
// from each recording's first sound, so the capture is compared as if it
// started when the reference did.
namespace audio_analysis {

inline constexpr int sample_rate = 44100;
inline constexpr int channel_count = 2;

// 16-bit samples, channels interleaved.
struct Audio {
    std::vector<std::int16_t> samples;

    std::size_t frames() const {
        return samples.size() / channel_count;
    }
};

// Reads a 16-bit stereo WAV or FLAC file at sample_rate. Throws
// std::runtime_error for any other file.
Audio readAudio(const std::filesystem::path& path);

// Writes 16-bit stereo FLAC for a .flac path and WAV otherwise. Throws
// std::runtime_error when the file cannot be written.
void writeAudio(const std::filesystem::path& path, const Audio& audio);

// The comparison's tolerances and resolution. A window is about 5 ms; the
// windows start pad_ms before the first sound.
inline constexpr int window_samples = 220;
inline constexpr double window_ms = 1000.0 * window_samples / sample_rate;
inline constexpr double pad_ms = 1000.0;
// Largest accepted timing shift against the reference.
inline constexpr double tau_ms = 10.0;
// Largest accepted difference of a window's level or click.
inline constexpr double level_delta_db = 10.0;
// Largest accepted difference of a spectral peak.
inline constexpr double peak_delta_db = 23.7;
// Largest accepted difference of the guide's repetition period.
inline constexpr double drift_limit_ms = 0.05;
// Spectral peaks come from Hann-windowed FFTs of this many samples, one
// starting at every window.
inline constexpr std::size_t fft_size = 8192;
// The reference must peak at or below this level, so that a louder capture
// is measured instead of clipped.
inline constexpr double reference_peak_limit_dbfs = -6.0;

// Where the drift check finds the guide's repetitions.
struct Guide {
    double first_repetition_ms{};
    double period_ms{};
    int repetitions{};
};

enum class FailureKind {
    TooQuiet,
    TooLoud,
    Click,
    Clipped,
    PeakTooQuiet,
    PeakTooLoud,
    Drift,
    ReferenceTooLoud,
    ReferencePeriod,
};

// One failing stretch of consecutive windows (or FFT frames), with the
// measured and allowed values of its worst window. Levels are in dBFS;
// Clipped measures samples at full scale and allows none; Drift and
// ReferencePeriod measure a period difference in ms, which is NaN when the
// audio ends before the guide's last repetition.
struct Failure {
    FailureKind kind{};
    double at_ms{};
    double length_ms{};
    double measured{};
    double allowed{};
    // The worst bin's frequency, for the spectral peak kinds.
    double frequency_hz{};
};

// One window's values. A limit or excess is NaN where its check does not
// apply. The peak columns describe the FFT frame centred in this window: the
// largest excess over the allowed level among its peaks, and that peak's
// frequency.
struct WindowValues {
    double reference_db{};
    double capture_db{};
    double quiet_limit_db{};
    double loud_limit_db{};
    double capture_click_db{};
    double click_limit_db{};
    double peak_quiet_excess_db{};
    double peak_quiet_hz{};
    double peak_loud_excess_db{};
    double peak_loud_hz{};
};

struct Comparison {
    // Sorted by time.
    std::vector<Failure> failures;
    // The largest excess of each check over its allowed value, in dB; a
    // negative value is the margin left. Empty when the check found nothing
    // to compare.
    std::optional<double> worst_quiet_db;
    std::optional<double> worst_loud_db;
    std::optional<double> worst_click_db;
    std::optional<double> worst_peak_quiet_db;
    std::optional<double> worst_peak_loud_db;
    double reference_peak_dbfs{};
    double capture_peak_dbfs{};
    // The guide's fitted repetition period; empty when the audio ends
    // before its last repetition.
    std::optional<double> reference_period_ms;
    std::optional<double> capture_period_ms;
    std::vector<WindowValues> windows;
};

Comparison compare(
    const Audio& reference,
    const Audio& capture,
    const Guide& guide
);

// One line: the failure's kind, time, length, and measured and allowed values.
std::string describe(const Failure& failure);

// One row per window. Throws
// std::runtime_error when the file cannot be written.
void writeWindowCsv(
    const std::filesystem::path& path,
    const Comparison& comparison
);

} // namespace audio_analysis
