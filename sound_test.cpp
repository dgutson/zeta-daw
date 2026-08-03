#include "configuration.hpp"

#include <fluidsynth.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr const char* default_config_path = "/etc/zeta-daw/zeta.yaml";
constexpr int test_channel = 0;
constexpr int low_key = 36;
constexpr int high_key = 84;
constexpr int test_velocity = 110;
constexpr double control_gain = 0.2;
constexpr auto case_settle_time = 500ms;
constexpr auto single_note_hold_time = 600ms;
constexpr auto single_note_release_time = 1000ms;
constexpr int single_note_attempts = 12;
constexpr int diagnostic_audio_period_size = 512;
constexpr int stress_probe_key = 36;
constexpr int stress_probe_velocity = 110;
constexpr int stress_background_velocity = 1;
constexpr auto stress_probe_hold_time = 600ms;
constexpr auto stress_probe_gap_time = 1000ms;
constexpr int stress_probe_repetitions = 30;
constexpr auto stress_release_time = 2000ms;
constexpr int stress_load_channels[]{2, 4, 8};
constexpr int stress_background_keys[]{36, 40, 43, 48, 52, 55, 60, 64};

enum class Direction : std::uint8_t {
    LowToHigh,
    HighToLow,
};

enum class Assessment : std::uint8_t {
    Clean,
    Spark,
    Unsure,
};

enum class ClickPhase : std::uint8_t {
    Clean,
    Attack,
    Release,
    Unsure,
};

enum class StressArtifact : std::uint8_t {
    Clean,
    Click,
    Sustained,
    Both,
    Unsure,
};

struct Pattern {
    const char* name;
    std::chrono::milliseconds first_hold;
    std::chrono::milliseconds transition;
    std::chrono::milliseconds second_hold;
    std::chrono::milliseconds recovery;
    int repetitions;
    bool overlap;
};

constexpr Pattern patterns[]{
    {
        .name = "slow-separated",
        .first_hold = 600ms,
        .transition = 300ms,
        .second_hold = 600ms,
        .recovery = 500ms,
        .repetitions = 2,
        .overlap = false,
    },
    {
        .name = "rapid-separated",
        .first_hold = 100ms,
        .transition = 50ms,
        .second_hold = 100ms,
        .recovery = 100ms,
        .repetitions = 8,
        .overlap = false,
    },
    {
        .name = "rapid-immediate",
        .first_hold = 100ms,
        .transition = 0ms,
        .second_hold = 100ms,
        .recovery = 100ms,
        .repetitions = 8,
        .overlap = false,
    },
    {
        .name = "rapid-overlap",
        .first_hold = 100ms,
        .transition = 75ms,
        .second_hold = 25ms,
        .recovery = 100ms,
        .repetitions = 8,
        .overlap = true,
    },
};

struct TestCase {
    double gain;
    Direction direction;
    const Pattern* pattern;
};

struct Observation {
    std::string preset_id;
    std::filesystem::path soundfont;
    int bank;
    int preset;
    TestCase test;
    Assessment assessment;
    int peak_active_voices;
};

struct SingleNoteObservation {
    std::string preset_id;
    std::filesystem::path soundfont;
    int bank;
    int preset;
    double gain;
    int key;
    int attempt;
    ClickPhase phase;
    int peak_active_voices;
};

struct StressObservation {
    std::string preset_id;
    std::filesystem::path soundfont;
    int bank;
    int preset;
    int period_size;
    int periods;
    int load_channels;
    StressArtifact artifact;
    int peak_active_voices;
    double peak_cpu_load;
};

struct StressMetrics {
    int peak_active_voices;
    double peak_cpu_load;
};

struct StressPhase {
    const zeta::SoundFontDefinition& preset;
    std::size_t preset_index;
    double gain;
    int load_channels;
};

struct SettingsDeleter {
    void operator()(fluid_settings_t* settings) const noexcept {
        delete_fluid_settings(settings);
    }
};

struct SynthDeleter {
    void operator()(fluid_synth_t* synth) const noexcept {
        delete_fluid_synth(synth);
    }
};

struct AudioDriverDeleter {
    void operator()(fluid_audio_driver_t* driver) const noexcept {
        delete_fluid_audio_driver(driver);
    }
};

using FluidSettings = std::unique_ptr<fluid_settings_t, SettingsDeleter>;
using FluidSynth = std::unique_ptr<fluid_synth_t, SynthDeleter>;
using FluidAudioDriver =
    std::unique_ptr<fluid_audio_driver_t, AudioDriverDeleter>;

void requireFluidOk(int result, const std::string& operation) {
    if (result != FLUID_OK) {
        throw std::runtime_error(operation);
    }
}

void configureAudio(
    fluid_settings_t* settings,
    const zeta::AudioConfig& audio
) {
    requireFluidOk(
        fluid_settings_setint(settings, "synth.threadsafe-api", 1),
        "Could not enable FluidSynth's thread-safe API"
    );
    requireFluidOk(
        fluid_settings_setnum(settings, "synth.gain", audio.gain),
        "Could not configure FluidSynth gain"
    );
    if (audio.period_size) {
        requireFluidOk(
            fluid_settings_setint(
                settings,
                "audio.period-size",
                *audio.period_size
            ),
            "Could not configure FluidSynth audio period size"
        );
    }
    if (audio.periods) {
        requireFluidOk(
            fluid_settings_setint(settings, "audio.periods", *audio.periods),
            "Could not configure FluidSynth audio periods"
        );
    }
    if (audio.driver) {
        requireFluidOk(
            fluid_settings_setstr(
                settings,
                "audio.driver",
                audio.driver->c_str()
            ),
            "Could not configure FluidSynth audio driver: " + *audio.driver
        );
    }
    if (audio.alsa_device) {
        requireFluidOk(
            fluid_settings_setstr(
                settings,
                "audio.alsa.device",
                audio.alsa_device->c_str()
            ),
            "Could not configure FluidSynth ALSA device: "
                + *audio.alsa_device
        );
    }
}

const char* directionName(Direction direction) {
    return direction == Direction::LowToHigh ? "low-to-high" : "high-to-low";
}

const char* assessmentName(Assessment assessment) {
    switch (assessment) {
    case Assessment::Clean:
        return "clean";
    case Assessment::Spark:
        return "spark";
    case Assessment::Unsure:
        return "unsure";
    }
    return "unknown";
}

const char* clickPhaseName(ClickPhase phase) {
    switch (phase) {
    case ClickPhase::Clean:
        return "clean";
    case ClickPhase::Attack:
        return "attack";
    case ClickPhase::Release:
        return "release";
    case ClickPhase::Unsure:
        return "unsure";
    }
    return "unknown";
}

const char* stressArtifactName(StressArtifact artifact) {
    switch (artifact) {
    case StressArtifact::Clean:
        return "clean";
    case StressArtifact::Click:
        return "click-or-frying";
    case StressArtifact::Sustained:
        return "sustained-rrrr";
    case StressArtifact::Both:
        return "both";
    case StressArtifact::Unsure:
        return "unsure";
    }
    return "unknown";
}

std::vector<double> testGains(double configured_gain) {
    std::vector<double> gains{configured_gain};
    if (configured_gain != control_gain) {
        gains.push_back(control_gain);
    }
    return gains;
}

std::vector<TestCase> makeTestCases(double configured_gain) {
    std::vector<TestCase> tests;
    for (const double gain : testGains(configured_gain)) {
        for (const Pattern& pattern : patterns) {
            tests.push_back({
                .gain = gain,
                .direction = Direction::LowToHigh,
                .pattern = &pattern,
            });
            tests.push_back({
                .gain = gain,
                .direction = Direction::HighToLow,
                .pattern = &pattern,
            });
        }
    }
    return tests;
}

std::chrono::milliseconds duration(const Pattern& pattern) {
    return pattern.repetitions
        * (pattern.first_hold + pattern.transition + pattern.second_hold
            + pattern.recovery);
}

void printCase(
    std::size_t index,
    std::size_t count,
    const zeta::SoundFontDefinition& preset,
    const TestCase& test
) {
    std::cout << "\n[" << index << '/' << count << "]"
              << " preset=" << preset.id
              << " bank=" << preset.bank
              << " program=" << preset.preset
              << " gain=" << test.gain
              << " direction=" << directionName(test.direction)
              << " timing=" << test.pattern->name
              << " keys=" << low_key << ',' << high_key
              << " velocity=" << test_velocity << '\n'
              << std::flush;
}

void listConfiguredPresets(const zeta::ApplicationConfig& config) {
    std::cout << "Configured presets:\n";
    for (std::size_t index = 0; index < config.soundfonts.size(); ++index) {
        const auto& preset = config.soundfonts[index];
        std::error_code error;
        const auto resolved = std::filesystem::canonical(preset.file, error);

        std::cout << "  " << index + 1 << ") " << preset.id
                  << " bank=" << preset.bank
                  << " program=" << preset.preset
                  << " file=" << preset.file;
        if (!error && resolved != preset.file) {
            std::cout << " -> " << resolved;
        }
        std::cout << '\n';
    }
}

void listMatrix(const zeta::ApplicationConfig& config) {
    listConfiguredPresets(config);
    const auto tests = makeTestCases(config.audio.gain);
    const std::size_t total = tests.size() * config.soundfonts.size();
    std::size_t index = 0;
    std::chrono::milliseconds total_duration{};

    std::cout << "\nTest matrix:\n";
    for (const auto& preset : config.soundfonts) {
        for (const auto& test : tests) {
            printCase(++index, total, preset, test);
            total_duration += duration(*test.pattern);
        }
    }
    std::cout << "\nCases: " << total
              << "\nApproximate playback time: "
              << std::chrono::duration_cast<std::chrono::seconds>(
                     total_duration
                 ).count()
              << " seconds, excluding prompts\n";
}

std::optional<std::vector<std::size_t>> choosePresets(
    const zeta::ApplicationConfig& config
) {
    while (true) {
        std::cout << "\nChoose a preset number, [a]ll, or [q]uit: "
                  << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer)) {
            return std::nullopt;
        }
        if (answer == "q" || answer == "Q") {
            return std::nullopt;
        }
        if (answer.empty() || answer == "a" || answer == "A") {
            std::vector<std::size_t> selected;
            for (std::size_t index = 0;
                 index < config.soundfonts.size();
                 ++index) {
                selected.push_back(index);
            }
            return selected;
        }
        std::size_t number{};
        const auto [end, error] = std::from_chars(
            answer.data(),
            answer.data() + answer.size(),
            number
        );
        if (error == std::errc{}
            && end == answer.data() + answer.size()
            && number >= 1
            && number <= config.soundfonts.size()) {
            return std::vector<std::size_t>{number - 1};
        }
        std::cout << "Invalid selection.\n";
    }
}

class TestSynth {
public:
    explicit TestSynth(
        const zeta::ApplicationConfig& config,
        std::optional<int> audio_period_size = std::nullopt,
        std::optional<int> audio_periods = std::nullopt
    ) {
        settings_.reset(new_fluid_settings());
        if (!settings_) {
            throw std::runtime_error("Could not create FluidSynth settings");
        }
        configureAudio(settings_.get(), config.audio);
        if (audio_period_size) {
            requireFluidOk(
                fluid_settings_setint(
                    settings_.get(),
                    "audio.period-size",
                    *audio_period_size
                ),
                "Could not configure FluidSynth audio period size"
            );
        }
        if (audio_periods) {
            requireFluidOk(
                fluid_settings_setint(
                    settings_.get(),
                    "audio.periods",
                    *audio_periods
                ),
                "Could not configure FluidSynth audio periods"
            );
        }

        printAudioSettings();

        synth_.reset(new_fluid_synth(settings_.get()));
        if (!synth_) {
            throw std::runtime_error("Could not create FluidSynth synth");
        }

        std::unordered_map<std::string, int> loaded_files;
        for (const auto& preset : config.soundfonts) {
            const auto path = preset.file.string();
            auto [loaded, inserted] = loaded_files.try_emplace(path, -1);
            if (inserted) {
                loaded->second =
                    fluid_synth_sfload(synth_.get(), path.c_str(), 0);
                if (loaded->second == FLUID_FAILED) {
                    throw std::runtime_error(
                        "Could not load SoundFont: " + path
                    );
                }
            }
            soundfont_ids_.push_back(loaded->second);
        }

        audio_driver_.reset(
            new_fluid_audio_driver(settings_.get(), synth_.get())
        );
        if (!audio_driver_) {
            throw std::runtime_error(
                "Could not create FluidSynth audio driver"
            );
        }
    }

    int play(
        const zeta::SoundFontDefinition& preset,
        std::size_t preset_index,
        const TestCase& test
    ) {
        select(preset_index, preset, test.gain);
        std::cout << " playing pattern now.\n" << std::flush;

        const int first =
            test.direction == Direction::LowToHigh ? low_key : high_key;
        const int second =
            test.direction == Direction::LowToHigh ? high_key : low_key;
        int peak_active_voices = 0;

        for (int repetition = 0;
             repetition < test.pattern->repetitions;
             ++repetition) {
            noteOn(first);
            updatePeak(peak_active_voices);
            std::this_thread::sleep_for(test.pattern->first_hold);

            if (test.pattern->overlap) {
                noteOn(second);
                updatePeak(peak_active_voices);
                std::this_thread::sleep_for(test.pattern->transition);
                noteOff(first);
                std::this_thread::sleep_for(test.pattern->second_hold);
                noteOff(second);
            } else {
                noteOff(first);
                std::this_thread::sleep_for(test.pattern->transition);
                noteOn(second);
                updatePeak(peak_active_voices);
                std::this_thread::sleep_for(test.pattern->second_hold);
                noteOff(second);
            }
            std::this_thread::sleep_for(test.pattern->recovery);
        }
        return peak_active_voices;
    }

    void select(
        std::size_t preset_index,
        const zeta::SoundFontDefinition& preset,
        double gain
    ) {
        std::cout << "Selecting preset..." << std::flush;
        fluid_synth_set_gain(synth_.get(), static_cast<float>(gain));
        requireFluidOk(
            fluid_synth_program_select(
                synth_.get(),
                test_channel,
                soundfont_ids_.at(preset_index),
                preset.bank,
                preset.preset
            ),
            "Could not select preset: " + preset.id
        );
        std::this_thread::sleep_for(case_settle_time);
    }

    int playSingleNote(int key, int attempt) {
        std::cout << "  attempt " << attempt << ": NOTE ON key=" << key
                  << std::flush;
        noteOn(key);
        int peak_active_voices = 0;
        updatePeak(peak_active_voices);
        std::this_thread::sleep_for(single_note_hold_time);

        std::cout << " ... NOTE OFF" << std::flush;
        noteOff(key);
        std::this_thread::sleep_for(single_note_release_time);
        std::cout << '\n';
        return peak_active_voices;
    }

    StressMetrics playStress(const StressPhase& phase) {
        fluid_synth_set_gain(synth_.get(), static_cast<float>(phase.gain));
        for (int channel = 0; channel <= phase.load_channels; ++channel) {
            selectChannel(phase.preset, phase.preset_index, channel);
        }
        std::this_thread::sleep_for(case_settle_time);

        std::cout << "Playing stress phase now; no per-note logging...\n"
                  << std::flush;

        StressMetrics metrics{};
        for (int channel = 1; channel <= phase.load_channels; ++channel) {
            for (const int key : stress_background_keys) {
                noteOn(channel, key, stress_background_velocity);
            }
        }
        updateMetrics(metrics);

        for (int repetition = 0;
             repetition < stress_probe_repetitions;
             ++repetition) {
            noteOn(test_channel, stress_probe_key, stress_probe_velocity);
            std::this_thread::sleep_for(stress_probe_hold_time);
            updateMetrics(metrics);

            noteOff(test_channel, stress_probe_key);
            std::this_thread::sleep_for(stress_probe_gap_time);
            updateMetrics(metrics);
        }

        for (int channel = 1; channel <= phase.load_channels; ++channel) {
            for (const int key : stress_background_keys) {
                noteOff(channel, key);
            }
        }
        std::this_thread::sleep_for(stress_release_time);
        updateMetrics(metrics);

        return metrics;
    }

    int audioPeriodSize() const {
        int period_size = 0;
        requireFluidOk(
            fluid_settings_getint(
                settings_.get(),
                "audio.period-size",
                &period_size
            ),
            "Could not read FluidSynth audio period size"
        );
        return period_size;
    }

    int audioPeriods() const {
        int periods = 0;
        requireFluidOk(
            fluid_settings_getint(settings_.get(), "audio.periods", &periods),
            "Could not read FluidSynth audio periods"
        );
        return periods;
    }

private:
    void printAudioSettings() const {
        char driver[64]{};
        int period_size = 0;
        int periods = 0;
        double sample_rate = 0.0;
        fluid_settings_copystr(
            settings_.get(),
            "audio.driver",
            driver,
            static_cast<int>(sizeof(driver))
        );
        fluid_settings_getint(
            settings_.get(),
            "audio.period-size",
            &period_size
        );
        fluid_settings_getint(settings_.get(), "audio.periods", &periods);
        fluid_settings_getnum(
            settings_.get(),
            "synth.sample-rate",
            &sample_rate
        );

        std::cout << "Effective audio settings: driver="
                  << (driver[0] == '\0' ? "unknown" : driver)
                  << " period-size=" << period_size
                  << " periods=" << periods
                  << " sample-rate=" << sample_rate << '\n';
    }

    void noteOn(int key) {
        noteOn(test_channel, key, test_velocity);
    }

    void noteOn(int channel, int key, int velocity) {
        requireFluidOk(
            fluid_synth_noteon(
                synth_.get(),
                channel,
                key,
                velocity
            ),
            "Could not start test note " + std::to_string(key)
        );
    }

    void noteOff(int key) {
        noteOff(test_channel, key);
    }

    void noteOff(int channel, int key) {
        // One-shot samples may finish before note-off and return FLUID_FAILED.
        fluid_synth_noteoff(synth_.get(), channel, key);
    }

    void selectChannel(
        const zeta::SoundFontDefinition& preset,
        std::size_t preset_index,
        int channel
    ) {
        requireFluidOk(
            fluid_synth_program_select(
                synth_.get(),
                channel,
                soundfont_ids_.at(preset_index),
                preset.bank,
                preset.preset
            ),
            "Could not select preset: " + preset.id
        );
    }

    void updatePeak(int& peak_active_voices) {
        peak_active_voices = std::max(
            peak_active_voices,
            fluid_synth_get_active_voice_count(synth_.get())
        );
    }

    void updateMetrics(StressMetrics& metrics) {
        updatePeak(metrics.peak_active_voices);
        metrics.peak_cpu_load = std::max(
            metrics.peak_cpu_load,
            fluid_synth_get_cpu_load(synth_.get())
        );
    }

    FluidSettings settings_;
    FluidSynth synth_;
    FluidAudioDriver audio_driver_;
    std::vector<int> soundfont_ids_;
};

struct Answer {
    std::optional<Assessment> assessment;
    bool replay{};
    bool skip_preset{};
    bool quit{};
};

Answer askForAssessment() {
    while (true) {
        std::cout
            << "Listen through the release tail. Result: "
               "[Enter/n] clean, [y] spark, [u] unsure, "
               "[r]eplay, ski[p] preset, [q]uit: "
            << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer)) {
            Answer result;
            result.quit = true;
            return result;
        }
        const char choice = answer.empty()
            ? 'n'
            : static_cast<char>(
                std::tolower(static_cast<unsigned char>(answer.front()))
            );
        switch (choice) {
        case 'n':
            return {.assessment = Assessment::Clean};
        case 'y':
            return {.assessment = Assessment::Spark};
        case 'u':
            return {.assessment = Assessment::Unsure};
        case 'r': {
            Answer result;
            result.replay = true;
            return result;
        }
        case 'p': {
            Answer result;
            result.skip_preset = true;
            return result;
        }
        case 'q': {
            Answer result;
            result.quit = true;
            return result;
        }
        default:
            std::cout << "Invalid result.\n";
        }
    }
}

struct SingleNoteAnswer {
    std::optional<ClickPhase> phase;
    bool quit{};
};

SingleNoteAnswer askForSingleNoteAssessment() {
    while (true) {
        std::cout
            << "Result: [Enter/n] clean, [a]ttack click, [r]elease click, "
               "[u]nsure, [q]uit: "
            << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer)) {
            return {.phase = std::nullopt, .quit = true};
        }
        const char choice = answer.empty()
            ? 'n'
            : static_cast<char>(
                std::tolower(static_cast<unsigned char>(answer.front()))
            );
        switch (choice) {
        case 'n':
            return {.phase = ClickPhase::Clean};
        case 'a':
            return {.phase = ClickPhase::Attack};
        case 'r':
            return {.phase = ClickPhase::Release};
        case 'u':
            return {.phase = ClickPhase::Unsure};
        case 'q':
            return {.phase = std::nullopt, .quit = true};
        default:
            std::cout << "Invalid result.\n";
        }
    }
}

struct StressAnswer {
    std::optional<StressArtifact> artifact;
    bool quit{};
};

StressAnswer askForStressAssessment() {
    while (true) {
        std::cout
            << "Result: [Enter/n] clean, [c]lick/frying, "
               "sustained [r]rrr, [b]oth, [u]nsure, [q]uit: "
            << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer)) {
            return {.artifact = std::nullopt, .quit = true};
        }
        const char choice = answer.empty()
            ? 'n'
            : static_cast<char>(
                std::tolower(static_cast<unsigned char>(answer.front()))
            );
        switch (choice) {
        case 'n':
            return {.artifact = StressArtifact::Clean};
        case 'c':
            return {.artifact = StressArtifact::Click};
        case 'r':
            return {.artifact = StressArtifact::Sustained};
        case 'b':
            return {.artifact = StressArtifact::Both};
        case 'u':
            return {.artifact = StressArtifact::Unsure};
        case 'q':
            return {.artifact = std::nullopt, .quit = true};
        default:
            std::cout << "Invalid result.\n";
        }
    }
}

void printSummary(
    std::size_t completed,
    const std::vector<Observation>& observations
) {
    std::cout << "\nDiagnostic summary\n"
              << "  completed cases: " << completed << '\n';
    if (observations.empty()) {
        std::cout << "  no sparks or uncertain cases reported\n";
        return;
    }

    for (const auto& observation : observations) {
        std::cout << "  " << assessmentName(observation.assessment)
                  << ": preset=" << observation.preset_id
                  << " file=" << observation.soundfont
                  << " bank=" << observation.bank
                  << " program=" << observation.preset
                  << " gain=" << observation.test.gain
                  << " direction="
                  << directionName(observation.test.direction)
                  << " timing=" << observation.test.pattern->name
                  << " keys=" << low_key << ',' << high_key
                  << " velocity=" << test_velocity
                  << " peak-active-voices="
                  << observation.peak_active_voices << '\n';
    }
}

void printSingleNoteSummary(
    std::size_t completed,
    const std::vector<SingleNoteObservation>& observations
) {
    std::cout << "\nSingle-note diagnostic summary\n"
              << "  completed notes: " << completed << '\n';
    if (observations.empty()) {
        std::cout << "  no clicks or uncertain notes reported\n";
        return;
    }

    for (const auto& observation : observations) {
        std::cout << "  " << clickPhaseName(observation.phase)
                  << ": preset=" << observation.preset_id
                  << " file=" << observation.soundfont
                  << " bank=" << observation.bank
                  << " program=" << observation.preset
                  << " gain=" << observation.gain
                  << " key=" << observation.key
                  << " attempt=" << observation.attempt
                  << " velocity=" << test_velocity
                  << " peak-active-voices="
                  << observation.peak_active_voices << '\n';
    }
}

void printStressSummary(
    const std::vector<StressObservation>& observations
) {
    std::cout << "\nRender-pressure diagnostic summary\n";
    if (observations.empty()) {
        std::cout << "  no completed phases\n";
        return;
    }

    for (const auto& observation : observations) {
        std::cout << "  " << stressArtifactName(observation.artifact)
                  << ": preset=" << observation.preset_id
                  << " file=" << observation.soundfont
                  << " bank=" << observation.bank
                  << " program=" << observation.preset
                  << " period-size=" << observation.period_size
                  << " periods=" << observation.periods
                  << " load-channels=" << observation.load_channels
                  << " peak-active-voices="
                  << observation.peak_active_voices
                  << " peak-cpu-load=" << observation.peak_cpu_load
                  << "%\n";
    }
}

int runInteractive(const zeta::ApplicationConfig& config) {
    listConfiguredPresets(config);
    const auto selected = choosePresets(config);
    if (!selected) {
        return 0;
    }

    const auto tests = makeTestCases(config.audio.gain);
    const std::size_t total = tests.size() * selected->size();
    std::cout << "\nFluidSynth runtime version: " << fluid_version_str()
              << "\nTest keys: " << low_key << " and " << high_key
              << "\nVelocity: " << test_velocity
              << "\nCases: " << total
              << "\nStop the regular Zeta process or service before this test."
              << "\nTurn the output down before starting."
              << "\nPress Enter to create the audio driver and begin, "
                 "or q to quit: "
              << std::flush;
    std::string start;
    if (!std::getline(std::cin, start) || start == "q" || start == "Q") {
        return 0;
    }

    TestSynth synth{config};
    std::vector<Observation> observations;
    std::size_t completed = 0;
    bool quit = false;

    for (const std::size_t preset_index : *selected) {
        const auto& preset = config.soundfonts[preset_index];
        bool skip_preset = false;
        for (const auto& test : tests) {
            while (true) {
                printCase(completed + 1, total, preset, test);
                const int peak_active_voices =
                    synth.play(preset, preset_index, test);
                std::cout << "Peak active voices observed: "
                          << peak_active_voices << '\n';

                const Answer answer = askForAssessment();
                if (answer.replay) {
                    continue;
                }
                if (answer.quit) {
                    quit = true;
                    break;
                }
                if (answer.skip_preset) {
                    skip_preset = true;
                    break;
                }
                const Assessment assessment =
                    answer.assessment.value_or(Assessment::Clean);
                ++completed;
                if (assessment != Assessment::Clean) {
                    observations.push_back({
                        .preset_id = preset.id,
                        .soundfont = preset.file,
                        .bank = preset.bank,
                        .preset = preset.preset,
                        .test = test,
                        .assessment = assessment,
                        .peak_active_voices = peak_active_voices,
                    });
                }
                break;
            }
            if (quit || skip_preset) {
                break;
            }
        }
        if (quit) {
            break;
        }
    }

    printSummary(completed, observations);
    return 0;
}

int runSingleNote(
    const zeta::ApplicationConfig& config,
    bool large_period
) {
    listConfiguredPresets(config);
    const auto selected = choosePresets(config);
    if (!selected) {
        return 0;
    }

    const auto gains = testGains(config.audio.gain);
    constexpr int keys[]{high_key, low_key};
    const std::size_t total = selected->size() * gains.size()
        * std::size(keys) * single_note_attempts;

    std::cout
        << "\nSingle-note onset/release diagnostic"
        << "\nFluidSynth runtime version: " << fluid_version_str()
        << "\nVelocity: " << test_velocity
        << "\nHold: " << single_note_hold_time.count() << " ms"
        << "\nRelease-listening window: "
        << single_note_release_time.count() << " ms"
        << "\nNotes: " << total
        << "\nStop the regular Zeta process or service before this test."
        << "\nPress Enter to create the audio driver and begin, "
           "or q to quit: "
        << std::flush;
    std::string start;
    if (!std::getline(std::cin, start) || start == "q" || start == "Q") {
        return 0;
    }

    TestSynth synth{
        config,
        large_period
            ? std::optional<int>{diagnostic_audio_period_size}
            : std::nullopt,
    };
    std::vector<SingleNoteObservation> observations;
    std::size_t completed = 0;
    bool quit = false;

    for (const std::size_t preset_index : *selected) {
        const auto& preset = config.soundfonts[preset_index];
        for (const double gain : gains) {
            for (const int key : keys) {
                std::cout << "\nPreset=" << preset.id
                          << " gain=" << gain
                          << " key=" << key << '\n';
                synth.select(preset_index, preset, gain);
                std::cout << " ready.\n";

                for (int attempt = 1;
                     attempt <= single_note_attempts;
                     ++attempt) {
                    const int peak_active_voices =
                        synth.playSingleNote(key, attempt);
                    const auto answer = askForSingleNoteAssessment();
                    if (answer.quit) {
                        quit = true;
                        break;
                    }

                    const ClickPhase phase =
                        answer.phase.value_or(ClickPhase::Clean);
                    ++completed;
                    if (phase != ClickPhase::Clean) {
                        observations.push_back({
                            .preset_id = preset.id,
                            .soundfont = preset.file,
                            .bank = preset.bank,
                            .preset = preset.preset,
                            .gain = gain,
                            .key = key,
                            .attempt = attempt,
                            .phase = phase,
                            .peak_active_voices = peak_active_voices,
                        });
                    }
                }
                if (quit) {
                    break;
                }
            }
            if (quit) {
                break;
            }
        }
        if (quit) {
            break;
        }
    }

    printSingleNoteSummary(completed, observations);
    return 0;
}

int runStress(
    const zeta::ApplicationConfig& config,
    std::optional<int> audio_period_size,
    std::optional<int> audio_periods
) {
    listConfiguredPresets(config);
    const auto selected = choosePresets(config);
    if (!selected) {
        return 0;
    }

    const std::size_t total =
        selected->size() * std::size(stress_load_channels);

    std::cout
        << "\nRender-pressure diagnostic"
        << "\nFluidSynth runtime version: " << fluid_version_str()
        << "\nConfigured gain: " << config.audio.gain
        << "\nProbe: key=" << stress_probe_key
        << " velocity=" << stress_probe_velocity
        << "\nBackground notes use velocity="
        << stress_background_velocity
        << " to add rendering work without a loud chord."
        << "\nEach phase lasts about "
        << (
               stress_probe_repetitions
               * (stress_probe_hold_time + stress_probe_gap_time)
               + stress_release_time
           ).count()
        / 1000
        << " seconds."
        << "\nStop the regular Zeta process or service before this test."
        << "\nTurn the output down before starting."
        << "\nPress Enter to create the audio driver and begin, "
           "or q to quit: "
        << std::flush;
    std::string start;
    if (!std::getline(std::cin, start) || start == "q" || start == "Q") {
        return 0;
    }

    TestSynth synth{
        config,
        audio_period_size,
        audio_periods,
    };
    const int period_size = synth.audioPeriodSize();
    const int periods = synth.audioPeriods();
    std::vector<StressObservation> observations;
    std::size_t phase_index = 0;
    bool quit = false;

    for (const std::size_t preset_index : *selected) {
        const auto& preset = config.soundfonts[preset_index];
        for (const int load_channels : stress_load_channels) {
            std::cout << "\n[" << ++phase_index << '/' << total << ']'
                      << " preset=" << preset.id
                      << " period-size=" << period_size
                      << " periods=" << periods
                      << " load-channels=" << load_channels << '\n';

            const StressMetrics metrics = synth.playStress({
                .preset = preset,
                .preset_index = preset_index,
                .gain = config.audio.gain,
                .load_channels = load_channels,
            });
            std::cout << "Peak active voices: "
                      << metrics.peak_active_voices
                      << "\nPeak FluidSynth CPU load: "
                      << metrics.peak_cpu_load << "%\n";

            const auto answer = askForStressAssessment();
            if (answer.quit) {
                quit = true;
                break;
            }
            observations.push_back({
                .preset_id = preset.id,
                .soundfont = preset.file,
                .bank = preset.bank,
                .preset = preset.preset,
                .period_size = period_size,
                .periods = periods,
                .load_channels = load_channels,
                .artifact =
                    answer.artifact.value_or(StressArtifact::Unsure),
                .peak_active_voices = metrics.peak_active_voices,
                .peak_cpu_load = metrics.peak_cpu_load,
            });
        }
        if (quit) {
            break;
        }
    }

    printStressSummary(observations);
    return 0;
}

void printUsage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " [--list|--single|--single-large-period"
                 "|--stress|--stress-period-128|--stress-period-256"
                 "|--stress-period-256-periods-4"
                 "|--stress-large-period] [config.yaml]\n";
}

} // namespace

int main(int argc, char** argv) {
    bool list_only = false;
    bool single_note = false;
    bool stress = false;
    bool large_period = false;
    std::optional<int> stress_period_size;
    std::optional<int> stress_periods;
    std::string config_path = default_config_path;

    if (argc >= 2
        && (std::string{argv[1]} == "--list"
            || std::string{argv[1]} == "--single"
            || std::string{argv[1]} == "--single-large-period"
            || std::string{argv[1]} == "--stress"
            || std::string{argv[1]} == "--stress-period-128"
            || std::string{argv[1]} == "--stress-period-256"
            || std::string{argv[1]}
                == "--stress-period-256-periods-4"
            || std::string{argv[1]} == "--stress-large-period")) {
        list_only = std::string{argv[1]} == "--list";
        single_note = std::string{argv[1]} == "--single"
            || std::string{argv[1]} == "--single-large-period";
        stress = std::string{argv[1]} == "--stress"
            || std::string{argv[1]} == "--stress-period-128"
            || std::string{argv[1]} == "--stress-period-256"
            || std::string{argv[1]}
                == "--stress-period-256-periods-4"
            || std::string{argv[1]} == "--stress-large-period";
        large_period = std::string{argv[1]}
            == "--single-large-period";
        if (std::string{argv[1]} == "--stress-period-128") {
            stress_period_size = 128;
        } else if (std::string{argv[1]} == "--stress-period-256") {
            stress_period_size = 256;
        } else if (
            std::string{argv[1]} == "--stress-period-256-periods-4"
        ) {
            stress_period_size = 256;
            stress_periods = 4;
        } else if (std::string{argv[1]} == "--stress-large-period") {
            stress_period_size = diagnostic_audio_period_size;
        }
        if (argc == 3) {
            config_path = argv[2];
        } else if (argc > 3) {
            printUsage(argv[0]);
            return 1;
        }
    } else if (argc == 2) {
        config_path = argv[1];
    } else if (argc > 2) {
        printUsage(argv[0]);
        return 1;
    }

    try {
        const auto config = zeta::loadConfiguration(config_path);
        if (list_only) {
            listMatrix(config);
            return 0;
        }
        if (single_note) {
            return runSingleNote(config, large_period);
        }
        if (stress) {
            return runStress(
                config,
                stress_period_size,
                stress_periods
            );
        }
        return runInteractive(config);
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
