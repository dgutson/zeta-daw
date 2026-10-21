#include "audio_analysis.hpp"

#include <sndfile.hh>

// Every transform here is one-dimensional, which pocketfft computes on the
// calling thread.
#define POCKETFFT_NO_MULTITHREADING
#include <pocketfft_hdronly.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <format>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace audio_analysis {

namespace {

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double minus_infinity = -std::numeric_limits<double>::infinity();

// Samples are divided by this, so the largest positive sample, 32767, is
// just below full scale.
constexpr double sample_scale = 32768.0;
constexpr int full_scale_sample = 32767;
// Magnitudes are counted from this one, so digital silence measures -240 dB.
constexpr double smallest_magnitude = 1e-12;
constexpr double silence_db = -240.0;
// The first sound is the first frame with a sample above this level. Where
// the reference is silent, the capture may sound up to it.
constexpr double silent_db = -80.0;
// The quiet check applies only where the reference is above this level.
constexpr double audible_db = -50.0;
constexpr auto pad_frames = static_cast<std::size_t>(pad_ms * sample_rate / 1000.0);
// ±τ in whole windows: 2 windows, 9.98 ms.
constexpr std::size_t tau_windows = static_cast<std::size_t>(tau_ms / window_ms);
constexpr std::size_t tau_span = 2 * tau_windows + 1;

constexpr std::size_t bin_count = fft_size / 2 + 1;
constexpr double bin_hz = static_cast<double>(sample_rate) / fft_size;
// A spectral peak is a bin at least as high as the two bins on each side,
// above peak_floor_db, within peak_range_db of its frame's highest bin, and
// at lowest_peak_hz or above.
constexpr double peak_floor_db = -70.0;
constexpr double peak_range_db = 40.0;
constexpr double lowest_peak_hz = 30.0;
constexpr std::size_t peak_neighbours = 2;
constexpr std::size_t last_peak_bin = bin_count - 1 - peak_neighbours;
// A frame starts at a window; its centre lies this many windows later.
constexpr std::size_t frame_centre_windows = fft_size / 2 / window_samples;
constexpr double frame_centre_ms = 1000.0 * (fft_size / 2) / sample_rate;

// The guide is timed below this frequency, where only its bass line sounds,
// at a twentieth of the sample rate.
constexpr double guide_lowpass_hz = 150.0;
constexpr std::size_t guide_decimation = 20;
constexpr double guide_rate = static_cast<double>(sample_rate) / guide_decimation;
// Each repetition is searched within this distance of where the previous
// one was found, which excludes the periodic side peaks of the bass notes.
constexpr double guide_search_ms = 2.0;

double sampleValue(std::int16_t sample) {
    return sample / sample_scale;
}

double toDb(double magnitude) {
    return 20.0 * std::log10(std::max(magnitude, smallest_magnitude));
}

double windowStartMs(std::size_t window) {
    return static_cast<double>(window) * window_ms - pad_ms;
}

std::size_t firstSound(const Audio& audio) {
    const double threshold = std::pow(10.0, silent_db / 20.0);
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        if (std::abs(sampleValue(audio.samples[i])) > threshold) {
            return i / channel_count;
        }
    }
    return 0;
}

double monoSample(const Audio& audio, std::size_t frame) {
    const auto left = sampleValue(audio.samples[frame * channel_count]);
    const auto right = sampleValue(audio.samples[frame * channel_count + 1]);
    return (left + right) / 2.0;
}

// The mono mix from pad_frames before the first sound, with zeros where the
// audio starts later than that.
std::vector<double> paddedMono(const Audio& audio, std::size_t first_sound) {
    const auto leading_zeros = first_sound < pad_frames ? pad_frames - first_sound : 0;
    const auto start = first_sound < pad_frames ? 0 : first_sound - pad_frames;
    std::vector<double> mono(leading_zeros, 0.0);
    mono.reserve(leading_zeros + audio.frames() - start);
    for (auto frame = start; frame < audio.frames(); ++frame) {
        mono.push_back(monoSample(audio, frame));
    }
    return mono;
}

double peakDbfs(const Audio& audio) {
    int peak = 0;
    for (const auto sample : audio.samples) {
        peak = std::max(peak, std::abs(static_cast<int>(sample)));
    }
    return toDb(peak / sample_scale);
}

// Collects consecutive failing windows (or FFT frames) into one Failure
// each, keeping the measured and allowed values of the worst one, and
// tracks the largest excess over every applicable index.
class Stretches {
public:
    Stretches(FailureKind kind, double first_index_ms)
        : kind_(kind),
          first_index_ms_(first_index_ms) {
    }

    // Indices must increase from call to call. An excess of minus infinity
    // means that the check does not apply at this index.
    void observe(
        std::size_t index,
        double excess,
        double measured,
        double allowed,
        double frequency_hz = 0.0
    ) {
        if (excess == minus_infinity) {
            close();
            return;
        }
        if (!worst_ || excess > *worst_) {
            worst_ = excess;
        }
        if (excess <= 0.0) {
            close();
            return;
        }
        if (open_ && index != last_ + 1) {
            close();
        }
        if (!open_) {
            open_ = true;
            first_ = index;
            worst_excess_ = minus_infinity;
        }
        last_ = index;
        if (excess > worst_excess_) {
            worst_excess_ = excess;
            current_ = {
                .kind = kind_,
                .measured = measured,
                .allowed = allowed,
                .frequency_hz = frequency_hz,
            };
        }
    }

    void finish(Comparison& comparison, std::optional<double>& worst) {
        close();
        comparison.failures.insert(comparison.failures.end(), failures_.begin(), failures_.end());
        worst = worst_;
    }

private:
    void close() {
        if (!open_) {
            return;
        }
        open_ = false;
        current_.at_ms = first_index_ms_ + static_cast<double>(first_) * window_ms;
        current_.length_ms = static_cast<double>(last_ - first_ + 1) * window_ms;
        failures_.push_back(current_);
    }

    FailureKind kind_;
    double first_index_ms_;
    std::vector<Failure> failures_;
    std::optional<double> worst_;
    bool open_{};
    std::size_t first_{};
    std::size_t last_{};
    double worst_excess_{};
    Failure current_{};
};

// The lowest or highest value within ±τ of each index.
template <typename Pick>
std::vector<double> withinTau(const std::vector<double>& values, Pick pick) {
    std::vector<double> result(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto first = i >= tau_windows ? i - tau_windows : 0;
        const auto last = std::min(values.size() - 1, i + tau_windows);
        auto value = values[first];
        for (auto j = first + 1; j <= last; ++j) {
            value = pick(value, values[j]);
        }
        result[i] = value;
    }
    return result;
}

struct WindowLevels {
    std::vector<double> level_db;
    std::vector<double> click_db;
};

// Each window's RMS level and largest second difference
// |x[n] - 2x[n-1] + x[n-2]|.
WindowLevels windowLevels(const std::vector<double>& mono) {
    const auto count = mono.size() / window_samples;
    WindowLevels levels;
    levels.level_db.reserve(count);
    levels.click_db.reserve(count);
    for (std::size_t window = 0; window < count; ++window) {
        double energy = 0.0;
        double click = 0.0;
        for (auto n = window * window_samples; n < (window + 1) * window_samples; ++n) {
            energy += mono[n] * mono[n];
            if (n >= 2) {
                click = std::max(click, std::abs(mono[n] - 2.0 * mono[n - 1] + mono[n - 2]));
            }
        }
        levels.level_db.push_back(toDb(std::sqrt(energy / window_samples)));
        levels.click_db.push_back(toDb(click));
    }
    return levels;
}

// A window fails when it is more than level_delta_db below the lowest
// reference level within ±τ (where the reference is audible), above the
// highest reference level within ±τ (or above silence where the reference is
// silent), or when its second difference is that far above the reference's.
void compareWindows(
    const std::vector<double>& reference_mono,
    const std::vector<double>& capture_mono,
    Comparison& comparison
) {
    auto reference = windowLevels(reference_mono);
    auto capture = windowLevels(capture_mono);
    const auto count = std::max(reference.level_db.size(), capture.level_db.size());
    for (auto* levels : {&reference, &capture}) {
        levels->level_db.resize(count, silence_db);
        levels->click_db.resize(count, silence_db);
    }
    const auto lower = [](double a, double b) { return std::min(a, b); };
    const auto higher = [](double a, double b) { return std::max(a, b); };
    const auto lowest = withinTau(reference.level_db, lower);
    const auto highest = withinTau(reference.level_db, higher);
    const auto highest_click = withinTau(reference.click_db, higher);

    Stretches quiet(FailureKind::TooQuiet, -pad_ms);
    Stretches loud(FailureKind::TooLoud, -pad_ms);
    Stretches click(FailureKind::Click, -pad_ms);
    comparison.windows.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto& window = comparison.windows[i];
        window.reference_db = reference.level_db[i];
        window.capture_db = capture.level_db[i];
        window.capture_click_db = capture.click_db[i];
        window.quiet_limit_db = lowest[i] > audible_db ? lowest[i] - level_delta_db : nan;
        window.loud_limit_db = std::max(highest[i] + level_delta_db, silent_db);
        window.click_limit_db = std::max(highest_click[i] + level_delta_db, silent_db);
        window.peak_quiet_excess_db = nan;
        window.peak_quiet_hz = nan;
        window.peak_loud_excess_db = nan;
        window.peak_loud_hz = nan;

        const auto quiet_excess = std::isnan(window.quiet_limit_db)
            ? minus_infinity
            : window.quiet_limit_db - window.capture_db;
        quiet.observe(i, quiet_excess, window.capture_db, window.quiet_limit_db);
        loud.observe(
            i,
            window.capture_db - window.loud_limit_db,
            window.capture_db,
            window.loud_limit_db
        );
        click.observe(
            i,
            window.capture_click_db - window.click_limit_db,
            window.capture_click_db,
            window.click_limit_db
        );
    }
    quiet.finish(comparison, comparison.worst_quiet_db);
    loud.finish(comparison, comparison.worst_loud_db);
    click.finish(comparison, comparison.worst_click_db);
}

// Hann-windowed power spectra of a padded mono mix, one FFT frame starting
// at every window, in single precision. Powers order bins as their levels in
// dB do, so peaks are found on powers and only their levels are computed.
class Spectra {
public:
    explicit Spectra(const std::vector<double>& mono)
        : mono_(mono.begin(), mono.end()),
          hann_(fft_size),
          windowed_(fft_size),
          spectrum_(bin_count) {
        // numpy.hanning's formula, symmetric over fft_size points.
        double sum = 0.0;
        for (std::size_t i = 0; i < fft_size; ++i) {
            const auto n = 2.0 * static_cast<double>(i) + 1.0 - fft_size;
            const auto value = 0.5 + 0.5 * std::cos(std::numbers::pi * n / (fft_size - 1));
            hann_[i] = static_cast<float>(value);
            sum += hann_[i];
        }
        // A full-scale sine measures 0 dB.
        scale_ = static_cast<float>(sum / 2.0);
    }

    std::size_t frames() const {
        return mono_.size() < fft_size ? 0 : (mono_.size() - fft_size) / window_samples + 1;
    }

    // Frame frame's power in every bin: zero for silence and past the last
    // frame.
    void compute(std::size_t frame, std::vector<float>& power) {
        std::fill(power.begin(), power.end(), 0.0F);
        if (frame >= frames()) {
            return;
        }
        const auto* const samples = mono_.data() + frame * window_samples;
        if (std::all_of(samples, samples + fft_size, [](float sample) { return sample == 0.0F; })) {
            return;
        }
        for (std::size_t i = 0; i < fft_size; ++i) {
            windowed_[i] = samples[i] * hann_[i];
        }
        pocketfft::r2c<float>(
            {fft_size},
            {sizeof(float)},
            {sizeof(std::complex<float>)},
            0,
            pocketfft::FORWARD,
            windowed_.data(),
            spectrum_.data(),
            1.0F
        );
        for (std::size_t bin = 0; bin < bin_count; ++bin) {
            power[bin] = std::norm(spectrum_[bin]);
        }
    }

    double db(float power) const {
        return toDb(std::sqrt(static_cast<double>(power)) / scale_);
    }

    float power(double db) const {
        const auto magnitude = std::pow(10.0, db / 20.0) * scale_;
        return static_cast<float>(magnitude * magnitude);
    }

private:
    std::vector<float> mono_;
    std::vector<float> hann_;
    std::vector<float> windowed_;
    std::vector<std::complex<float>> spectrum_;
    float scale_{};
};

bool isPeak(const std::vector<float>& power, std::size_t bin, float threshold) {
    for (std::size_t distance = 1; distance <= peak_neighbours; ++distance) {
        if (power[bin] < power[bin - distance] || power[bin] < power[bin + distance]) {
            return false;
        }
    }
    return power[bin] > threshold;
}

float peakThreshold(const std::vector<float>& power, float floor_power) {
    const auto range = static_cast<float>(std::pow(10.0, -peak_range_db / 10.0));
    return std::max(floor_power, *std::max_element(power.begin(), power.end()) * range);
}

float nearBins(const std::vector<float>& power, std::size_t bin) {
    return std::max({power[bin - 1], power[bin], power[bin + 1]});
}

// A frame fails when a reference peak has no capture level within one bin
// that is less than peak_delta_db below the lowest reference level within
// ±τ (too quiet), or when a capture peak is more than peak_delta_db above the
// highest reference level within ±τ and one bin (too loud).
void comparePeaks(
    const std::vector<double>& reference_mono,
    const std::vector<double>& capture_mono,
    Comparison& comparison
) {
    Spectra reference(reference_mono);
    Spectra capture(capture_mono);
    const auto count = std::max(reference.frames(), capture.frames());
    // Reference frames f - τ through f + τ, at index frame % tau_span.
    std::array<std::vector<float>, tau_span> nearby;
    for (auto& frame : nearby) {
        frame.resize(bin_count);
    }
    std::vector<float> captured(bin_count);
    const auto first_peak_bin = static_cast<std::size_t>(std::ceil(lowest_peak_hz / bin_hz));
    const auto floor_power = reference.power(peak_floor_db);
    Stretches quiet(FailureKind::PeakTooQuiet, -pad_ms + frame_centre_ms);
    Stretches loud(FailureKind::PeakTooLoud, -pad_ms + frame_centre_ms);
    for (std::size_t frame = 0; frame < count; ++frame) {
        const auto first_new = frame == 0 ? 0 : frame + tau_windows;
        for (auto f = first_new; f <= frame + tau_windows && f < count; ++f) {
            reference.compute(f, nearby[f % tau_span]);
        }
        capture.compute(frame, captured);
        const auto first_row = frame >= tau_windows ? frame - tau_windows : 0;
        const auto last_row = std::min(count - 1, frame + tau_windows);
        const auto& current = nearby[frame % tau_span];
        const auto reference_threshold = peakThreshold(current, floor_power);
        const auto capture_threshold = peakThreshold(captured, floor_power);

        double quiet_excess = minus_infinity;
        double quiet_measured = nan;
        double quiet_allowed = nan;
        std::size_t quiet_bin = 0;
        double loud_excess = minus_infinity;
        double loud_measured = nan;
        double loud_allowed = nan;
        std::size_t loud_bin = 0;
        for (auto bin = first_peak_bin; bin <= last_peak_bin; ++bin) {
            if (isPeak(current, bin, reference_threshold)) {
                auto lowest = current[bin];
                for (auto row = first_row; row <= last_row; ++row) {
                    lowest = std::min(lowest, nearby[row % tau_span][bin]);
                }
                const auto measured = std::max(reference.db(nearBins(captured, bin)), peak_floor_db);
                const auto allowed = reference.db(lowest) - peak_delta_db;
                if (allowed - measured > quiet_excess) {
                    quiet_excess = allowed - measured;
                    quiet_measured = measured;
                    quiet_allowed = allowed;
                    quiet_bin = bin;
                }
            }
            if (isPeak(captured, bin, capture_threshold)) {
                auto highest = 0.0F;
                for (auto row = first_row; row <= last_row; ++row) {
                    highest = std::max(highest, nearBins(nearby[row % tau_span], bin));
                }
                const auto measured = reference.db(captured[bin]);
                const auto allowed = std::max(reference.db(highest), peak_floor_db) + peak_delta_db;
                if (measured - allowed > loud_excess) {
                    loud_excess = measured - allowed;
                    loud_measured = measured;
                    loud_allowed = allowed;
                    loud_bin = bin;
                }
            }
        }
        quiet.observe(frame, quiet_excess, quiet_measured, quiet_allowed, quiet_bin * bin_hz);
        loud.observe(frame, loud_excess, loud_measured, loud_allowed, loud_bin * bin_hz);

        const auto window = frame + frame_centre_windows;
        if (window < comparison.windows.size()) {
            auto& values = comparison.windows[window];
            if (quiet_excess != minus_infinity) {
                values.peak_quiet_excess_db = quiet_excess;
                values.peak_quiet_hz = quiet_bin * bin_hz;
            }
            if (loud_excess != minus_infinity) {
                values.peak_loud_excess_db = loud_excess;
                values.peak_loud_hz = loud_bin * bin_hz;
            }
        }
    }
    quiet.finish(comparison, comparison.worst_peak_quiet_db);
    loud.finish(comparison, comparison.worst_peak_loud_db);
}

void checkClipping(const Audio& capture, std::size_t first_sound, Comparison& comparison) {
    std::size_t clipped = 0;
    std::size_t first = 0;
    std::size_t last = 0;
    for (std::size_t i = 0; i < capture.samples.size(); ++i) {
        if (std::abs(static_cast<int>(capture.samples[i])) >= full_scale_sample) {
            if (clipped == 0) {
                first = i / channel_count;
            }
            last = i / channel_count;
            ++clipped;
        }
    }
    if (clipped == 0) {
        return;
    }
    const auto frame_ms = 1000.0 / sample_rate;
    comparison.failures.push_back({
        .kind = FailureKind::Clipped,
        .at_ms = (static_cast<double>(first) - static_cast<double>(first_sound)) * frame_ms,
        .length_ms = static_cast<double>(last - first + 1) * frame_ms,
        .measured = static_cast<double>(clipped),
        .allowed = 0.0,
    });
}

void checkReferencePeak(const Audio& reference, std::size_t first_sound, Comparison& comparison) {
    if (comparison.reference_peak_dbfs <= reference_peak_limit_dbfs) {
        return;
    }
    const auto loudest = std::max_element(
        reference.samples.begin(),
        reference.samples.end(),
        [](std::int16_t a, std::int16_t b) { return std::abs(a) < std::abs(b); }
    );
    const auto frame = static_cast<std::size_t>(loudest - reference.samples.begin()) / channel_count;
    comparison.failures.push_back({
        .kind = FailureKind::ReferenceTooLoud,
        .at_ms = (static_cast<double>(frame) - static_cast<double>(first_sound)) * 1000.0 / sample_rate,
        .measured = comparison.reference_peak_dbfs,
        .allowed = reference_peak_limit_dbfs,
    });
}

struct Biquad {
    double b0{};
    double b1{};
    double b2{};
    double a1{};
    double a2{};
};

// A fourth-order Butterworth low-pass as two second-order sections, by the
// bilinear transform with the cutoff prewarped, as scipy.signal.butter
// designs it.
std::array<Biquad, 2> guideLowPass() {
    const auto k = std::tan(std::numbers::pi * guide_lowpass_hz / sample_rate);
    std::array<Biquad, 2> sections;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        // Each section holds one conjugate pair of the prototype's poles.
        const auto q = 1.0 / (2.0 * std::sin(std::numbers::pi * static_cast<double>(2 * i + 1) / 8.0));
        const auto norm = 1.0 / (1.0 + k / q + k * k);
        sections[i] = {
            .b0 = k * k * norm,
            .b1 = 2.0 * k * k * norm,
            .b2 = k * k * norm,
            .a1 = 2.0 * (k * k - 1.0) * norm,
            .a2 = (1.0 - k / q + k * k) * norm,
        };
    }
    return sections;
}

// The mono mix low-passed (single pass, from the first frame) and decimated.
std::vector<double> guideBand(const Audio& audio) {
    const auto sections = guideLowPass();
    // Transposed direct form II state of each section.
    std::array<std::array<double, 2>, 2> state{};
    std::vector<double> band;
    band.reserve(audio.frames() / guide_decimation + 1);
    for (std::size_t frame = 0; frame < audio.frames(); ++frame) {
        auto x = monoSample(audio, frame);
        for (std::size_t s = 0; s < sections.size(); ++s) {
            const auto& c = sections[s];
            const auto y = c.b0 * x + state[s][0];
            state[s][0] = c.b1 * x - c.a1 * y + state[s][1];
            state[s][1] = c.b2 * x - c.a2 * y;
            x = y;
        }
        if (frame % guide_decimation == 0) {
            band.push_back(x);
        }
    }
    return band;
}

// The guide's repetition period in ms: each repetition of the low band is
// cross-correlated with the first, near where the previous one was found,
// refined on a parabola, and a least-squares line through the repetitions'
// starts gives the period. Empty when the audio ends before the last
// repetition.
std::optional<double> guidePeriodMs(const Audio& audio, std::size_t first_sound, const Guide& guide) {
    const auto band = guideBand(audio);
    const auto start = (static_cast<double>(first_sound) + guide.first_repetition_ms * sample_rate / 1000.0)
        / guide_decimation;
    const auto period = guide.period_ms * guide_rate / 1000.0;
    const auto length = static_cast<std::ptrdiff_t>(period);
    const auto search = static_cast<std::ptrdiff_t>(std::nearbyint(guide_search_ms / 1000.0 * guide_rate));
    const auto first = static_cast<std::ptrdiff_t>(start);
    const auto size = static_cast<std::ptrdiff_t>(band.size());
    if (guide.repetitions < 2 || length < 1 || first + length > size) {
        return std::nullopt;
    }

    std::vector<double> starts;
    std::vector<double> correlation(static_cast<std::size_t>(2 * search + 1));
    double offset = 0.0;
    for (int repetition = 0; repetition < guide.repetitions; ++repetition) {
        const auto expected = start + repetition * period;
        // std::nearbyint rounds halves to even, as the prototype's round did.
        const auto base = static_cast<std::ptrdiff_t>(std::nearbyint(expected + offset));
        if (base - search < 0 || base + search + length > size) {
            return std::nullopt;
        }
        for (auto lag = -search; lag <= search; ++lag) {
            double sum = 0.0;
            for (std::ptrdiff_t j = 0; j < length; ++j) {
                sum += band[static_cast<std::size_t>(first + j)] * band[static_cast<std::size_t>(base + lag + j)];
            }
            correlation[static_cast<std::size_t>(lag + search)] = sum;
        }
        const auto best = static_cast<std::ptrdiff_t>(
            std::max_element(correlation.begin(), correlation.end()) - correlation.begin()
        );
        double fraction = 0.0;
        if (best > 0 && best < 2 * search) {
            const auto y0 = correlation[static_cast<std::size_t>(best - 1)];
            const auto y1 = correlation[static_cast<std::size_t>(best)];
            const auto y2 = correlation[static_cast<std::size_t>(best + 1)];
            const auto denominator = y0 - 2.0 * y1 + y2;
            fraction = denominator != 0.0 ? 0.5 * (y0 - y2) / denominator : 0.0;
        }
        const auto found = static_cast<double>(base + (best - search)) + fraction;
        starts.push_back(found);
        offset = found - expected;
    }

    const auto mean_repetition = (guide.repetitions - 1) / 2.0;
    double mean_start = 0.0;
    for (const auto value : starts) {
        mean_start += value;
    }
    mean_start /= guide.repetitions;
    double covariance = 0.0;
    double variance = 0.0;
    for (int repetition = 0; repetition < guide.repetitions; ++repetition) {
        const auto distance = repetition - mean_repetition;
        covariance += distance * (starts[static_cast<std::size_t>(repetition)] - mean_start);
        variance += distance * distance;
    }
    return covariance / variance / guide_rate * 1000.0;
}

void checkDrift(
    const Audio& reference,
    std::size_t reference_first_sound,
    const Audio& capture,
    std::size_t capture_first_sound,
    const Guide& guide,
    Comparison& comparison
) {
    comparison.reference_period_ms = guidePeriodMs(reference, reference_first_sound, guide);
    comparison.capture_period_ms = guidePeriodMs(capture, capture_first_sound, guide);
    const Failure base{
        .at_ms = guide.first_repetition_ms,
        .length_ms = guide.repetitions * guide.period_ms,
        .measured = nan,
        .allowed = drift_limit_ms,
    };
    const auto& reference_period = comparison.reference_period_ms;
    const auto& capture_period = comparison.capture_period_ms;
    if (!reference_period || std::abs(*reference_period - guide.period_ms) > drift_limit_ms) {
        auto failure = base;
        failure.kind = FailureKind::ReferencePeriod;
        if (reference_period) {
            failure.measured = *reference_period - guide.period_ms;
        }
        comparison.failures.push_back(failure);
    }
    if (!capture_period || (reference_period && std::abs(*capture_period - *reference_period) > drift_limit_ms)) {
        auto failure = base;
        failure.kind = FailureKind::Drift;
        if (capture_period) {
            failure.measured = *capture_period - *reference_period;
        }
        comparison.failures.push_back(failure);
    }
}

std::string seconds(double ms) {
    return std::format("{:.3f} s", ms / 1000.0);
}

std::string csvValue(double value) {
    return std::isnan(value) ? std::string() : std::format("{:.3f}", value);
}

} // namespace

Audio readAudio(const std::filesystem::path& path) {
    SndfileHandle file(path.string());
    if (file.error() != SF_ERR_NO_ERROR) {
        throw std::runtime_error(path.string() + ": " + file.strError());
    }
    if (file.samplerate() != sample_rate
        || file.channels() != channel_count
        || (file.format() & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) {
        throw std::runtime_error(path.string() + ": not 16-bit stereo at 44.1 kHz");
    }
    Audio audio;
    audio.samples.resize(static_cast<std::size_t>(file.frames()) * channel_count);
    if (file.readf(audio.samples.data(), file.frames()) != file.frames()) {
        throw std::runtime_error(path.string() + ": " + file.strError());
    }
    return audio;
}

void writeAudio(const std::filesystem::path& path, const Audio& audio) {
    const auto container = path.extension() == ".flac" ? SF_FORMAT_FLAC : SF_FORMAT_WAV;
    SndfileHandle file(path.string(), SFM_WRITE, container | SF_FORMAT_PCM_16, channel_count, sample_rate);
    if (file.error() != SF_ERR_NO_ERROR) {
        throw std::runtime_error(path.string() + ": " + file.strError());
    }
    // The reference is kept in git: compress it as much as FLAC can.
    double compression = 1.0;
    file.command(SFC_SET_COMPRESSION_LEVEL, &compression, sizeof(compression));
    const auto frames = static_cast<sf_count_t>(audio.frames());
    if (file.writef(audio.samples.data(), frames) != frames) {
        throw std::runtime_error(path.string() + ": " + file.strError());
    }
}

Comparison compare(const Audio& reference, const Audio& capture, const Guide& guide) {
    Comparison comparison;
    const auto reference_first_sound = firstSound(reference);
    const auto capture_first_sound = firstSound(capture);
    const auto reference_mono = paddedMono(reference, reference_first_sound);
    const auto capture_mono = paddedMono(capture, capture_first_sound);
    comparison.reference_peak_dbfs = peakDbfs(reference);
    comparison.capture_peak_dbfs = peakDbfs(capture);

    compareWindows(reference_mono, capture_mono, comparison);
    comparePeaks(reference_mono, capture_mono, comparison);
    checkClipping(capture, capture_first_sound, comparison);
    checkReferencePeak(reference, reference_first_sound, comparison);
    checkDrift(reference, reference_first_sound, capture, capture_first_sound, guide, comparison);
    std::stable_sort(
        comparison.failures.begin(),
        comparison.failures.end(),
        [](const Failure& a, const Failure& b) { return a.at_ms < b.at_ms; }
    );
    return comparison;
}

std::string describe(const Failure& failure) {
    const auto when = std::format("at {} for {:.1f} ms", seconds(failure.at_ms), failure.length_ms);
    switch (failure.kind) {
    case FailureKind::TooQuiet:
        return std::format(
            "too quiet {}: {:.1f} dBFS, lowest allowed {:.1f} dBFS",
            when, failure.measured, failure.allowed
        );
    case FailureKind::TooLoud:
        return std::format(
            "too loud {}: {:.1f} dBFS, highest allowed {:.1f} dBFS",
            when, failure.measured, failure.allowed
        );
    case FailureKind::Click:
        return std::format(
            "click {}: second difference {:.1f} dBFS, highest allowed {:.1f} dBFS",
            when, failure.measured, failure.allowed
        );
    case FailureKind::Clipped:
        return std::format("clipped {}: {:.0f} samples at full scale", when, failure.measured);
    case FailureKind::PeakTooQuiet:
        return std::format(
            "spectral peak too quiet {}, {:.0f} Hz: {:.1f} dBFS, lowest allowed {:.1f} dBFS",
            when, failure.frequency_hz, failure.measured, failure.allowed
        );
    case FailureKind::PeakTooLoud:
        return std::format(
            "spectral peak too loud {}, {:.0f} Hz: {:.1f} dBFS, highest allowed {:.1f} dBFS",
            when, failure.frequency_hz, failure.measured, failure.allowed
        );
    case FailureKind::Drift:
        if (std::isnan(failure.measured)) {
            return std::format("drift {}: the capture ends before the guide's last repetition", when);
        }
        return std::format(
            "drift {}: the guide's period differs from the reference's by {:.4f} ms, at most {:.2f} ms allowed",
            when, failure.measured, failure.allowed
        );
    case FailureKind::ReferenceTooLoud:
        return std::format(
            "reference too loud at {}: peak {:.1f} dBFS, highest allowed {:.1f} dBFS",
            seconds(failure.at_ms), failure.measured, failure.allowed
        );
    case FailureKind::ReferencePeriod:
        if (std::isnan(failure.measured)) {
            return std::format("reference period {}: the reference ends before the guide's last repetition", when);
        }
        return std::format(
            "reference period {}: the guide's period differs from the scripted one by {:.4f} ms, at most {:.2f} ms allowed",
            when, failure.measured, failure.allowed
        );
    }
    return "unknown failure";
}

void writeWindowCsv(const std::filesystem::path& path, const Comparison& comparison) {
    std::ofstream file(path);
    file << "time_ms,reference_db,capture_db,quiet_limit_db,loud_limit_db,capture_click_db,click_limit_db,"
            "peak_quiet_excess_db,peak_quiet_hz,peak_loud_excess_db,peak_loud_hz\n";
    for (std::size_t i = 0; i < comparison.windows.size(); ++i) {
        const auto& w = comparison.windows[i];
        file << std::format(
            "{:.3f},{:.3f},{:.3f},{},{:.3f},{:.3f},{:.3f},{},{},{},{}\n",
            windowStartMs(i), w.reference_db, w.capture_db, csvValue(w.quiet_limit_db), w.loud_limit_db,
            w.capture_click_db, w.click_limit_db, csvValue(w.peak_quiet_excess_db), csvValue(w.peak_quiet_hz),
            csvValue(w.peak_loud_excess_db), csvValue(w.peak_loud_hz)
        );
    }
    if (!file) {
        throw std::runtime_error(path.string() + ": cannot write");
    }
}

} // namespace audio_analysis
