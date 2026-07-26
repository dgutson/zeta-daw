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

enum class Direction : std::uint8_t {
    LowToHigh,
    HighToLow,
};

enum class Assessment : std::uint8_t {
    Clean,
    Spark,
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
    explicit TestSynth(const zeta::ApplicationConfig& config) {
        settings_.reset(new_fluid_settings());
        if (!settings_) {
            throw std::runtime_error("Could not create FluidSynth settings");
        }
        configureAudio(settings_.get(), config.audio);

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
        std::cout << "Clearing previous voices..." << std::flush;
        silence();
        fluid_synth_set_gain(synth_.get(), static_cast<float>(test.gain));
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

private:
    void noteOn(int key) {
        requireFluidOk(
            fluid_synth_noteon(
                synth_.get(),
                test_channel,
                key,
                test_velocity
            ),
            "Could not start test note " + std::to_string(key)
        );
    }

    void noteOff(int key) {
        // One-shot samples may finish before note-off and return FLUID_FAILED.
        fluid_synth_noteoff(synth_.get(), test_channel, key);
    }

    void silence() {
        fluid_synth_all_sounds_off(synth_.get(), test_channel);
    }

    void updatePeak(int& peak_active_voices) {
        peak_active_voices = std::max(
            peak_active_voices,
            fluid_synth_get_active_voice_count(synth_.get())
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

void printUsage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " [--list] [config.yaml]\n";
}

} // namespace

int main(int argc, char** argv) {
    bool list_only = false;
    std::string config_path = default_config_path;

    if (argc >= 2 && std::string{argv[1]} == "--list") {
        list_only = true;
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
        return runInteractive(config);
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
