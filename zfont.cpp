#include "configuration.hpp"

#include <fluidsynth.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr const char* default_config_path = "/etc/zeta-daw/zeta.yaml";
constexpr int audition_channel = 0;
constexpr int audition_key = 60;
constexpr int audition_velocity = 100;
constexpr auto audition_duration = std::chrono::seconds{1};

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

int run(const std::string& soundfont_path, const std::string& config_path) {
    const auto config = zeta::loadConfiguration(config_path);

    FluidSettings settings{new_fluid_settings()};
    if (!settings) {
        throw std::runtime_error("Could not create FluidSynth settings");
    }
    configureAudio(settings.get(), config.audio);

    FluidSynth synth{new_fluid_synth(settings.get())};
    if (!synth) {
        throw std::runtime_error("Could not create FluidSynth synth");
    }

    const int soundfont_id =
        fluid_synth_sfload(synth.get(), soundfont_path.c_str(), 0);
    if (soundfont_id == FLUID_FAILED) {
        throw std::runtime_error("Could not load SoundFont: " + soundfont_path);
    }

    FluidAudioDriver audio_driver{
        new_fluid_audio_driver(settings.get(), synth.get())
    };
    if (!audio_driver) {
        throw std::runtime_error("Could not create FluidSynth audio driver");
    }

    fluid_sfont_t* soundfont =
        fluid_synth_get_sfont_by_id(synth.get(), soundfont_id);
    if (!soundfont) {
        throw std::runtime_error(
            "Could not inspect SoundFont: " + soundfont_path
        );
    }

    const int bank_offset =
        fluid_synth_get_bank_offset(synth.get(), soundfont_id);
    fluid_sfont_iteration_start(soundfont);
    while (fluid_preset_t* preset = fluid_sfont_iteration_next(soundfont)) {
        const int bank = fluid_preset_get_banknum(preset) + bank_offset;
        const int preset_number = fluid_preset_get_num(preset);

        std::cout << std::setfill('0') << std::setw(3) << bank << '-'
                  << std::setw(3) << preset_number << ' '
                  << fluid_preset_get_name(preset) << '\n'
                  << std::flush;

        requireFluidOk(
            fluid_synth_program_select(
                synth.get(),
                audition_channel,
                soundfont_id,
                bank,
                preset_number
            ),
            "Could not select SoundFont preset"
        );
        requireFluidOk(
            fluid_synth_noteon(
                synth.get(),
                audition_channel,
                audition_key,
                audition_velocity
            ),
            "Could not start audition note"
        );
        std::this_thread::sleep_for(audition_duration);
        requireFluidOk(
            fluid_synth_noteoff(synth.get(), audition_channel, audition_key),
            "Could not stop audition note"
        );
    }

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " SOUNDFONT [config.yaml]\n";
        return 1;
    }

    try {
        const std::string config_path =
            argc == 3 ? argv[2] : default_config_path;
        return run(argv[1], config_path);
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
