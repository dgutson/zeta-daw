#include "synth_engine.hpp"

#include <fluidsynth.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <syncstream>
#include <unordered_map>

namespace zeta {
namespace {

struct SettingsDeleter {
    void operator()(fluid_settings_t* settings) const noexcept {
        if (settings) {
            delete_fluid_settings(settings);
        }
    }
};

struct SynthDeleter {
    void operator()(fluid_synth_t* synth) const noexcept {
        if (synth) {
            delete_fluid_synth(synth);
        }
    }
};

struct AudioDriverDeleter {
    void operator()(fluid_audio_driver_t* driver) const noexcept {
        if (driver) {
            delete_fluid_audio_driver(driver);
        }
    }
};

using FluidSettings = std::unique_ptr<fluid_settings_t, SettingsDeleter>;
using FluidSynth = std::unique_ptr<fluid_synth_t, SynthDeleter>;
using FluidAudioDriver = std::unique_ptr<fluid_audio_driver_t, AudioDriverDeleter>;

} // namespace

struct SynthEngine::Impl {
    FluidSettings settings;
    FluidSynth synth;
    FluidAudioDriver audio_driver;
    std::unordered_map<std::string, int> soundfont_ids;
    int midi_channel_count{};

    explicit Impl(const ApplicationConfig& config) {
        settings.reset(new_fluid_settings());
        if (!settings) {
            throw std::runtime_error("Could not create FluidSynth settings");
        }

        fluid_settings_setint(settings.get(), "synth.threadsafe-api", 1);
        fluid_settings_setnum(settings.get(), "synth.gain", 0.5);
        constexpr int fluidsynth_default_midi_channels = 16;
        midi_channel_count = std::max(
            fluidsynth_default_midi_channels,
            static_cast<int>(config.loop_slots.size()) + 1
        );
        const int channel_result = fluid_settings_setint(
            settings.get(),
            "synth.midi-channels",
            midi_channel_count
        );
        if (channel_result != FLUID_OK) {
            throw std::runtime_error(
                "Could not configure FluidSynth MIDI-channel count"
            );
        }

        synth.reset(new_fluid_synth(settings.get()));
        if (!synth) {
            throw std::runtime_error("Could not create FluidSynth synth");
        }

        std::unordered_map<std::string, int> loaded_files;
        for (const auto& definition : config.soundfonts) {
            const auto path = definition.file.string();
            auto [loaded, inserted] = loaded_files.try_emplace(path, -1);
            if (inserted) {
                loaded->second = fluid_synth_sfload(synth.get(), path.c_str(), 0);
                if (loaded->second == -1) {
                    throw std::runtime_error("Could not load SoundFont: " + path);
                }
            }
            soundfont_ids.emplace(definition.id, loaded->second);
        }

        audio_driver.reset(new_fluid_audio_driver(settings.get(), synth.get()));
        if (!audio_driver) {
            throw std::runtime_error("Could not create FluidSynth audio driver");
        }
    }
};

SynthEngine::SynthEngine(const ApplicationConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

SynthEngine::~SynthEngine() = default;

int SynthEngine::send(const MidiMessage& message, int channel) {
    switch (classifyMidiMessage(message.raw_type)) {
    case MidiMessageType::NoteOff:
        return noteOff(channel, message.key);
    case MidiMessageType::NoteOn:
        return noteOn(channel, message.key, message.velocity);
    case MidiMessageType::PolyphonicKeyPressure:
        return fluid_synth_key_pressure(
            impl_->synth.get(), channel, message.key, message.pressure
        );
    case MidiMessageType::ControlChange:
        return fluid_synth_cc(
            impl_->synth.get(), channel, message.control, message.value
        );
    case MidiMessageType::ProgramChange:
        return fluid_synth_program_change(
            impl_->synth.get(), channel, message.program
        );
    case MidiMessageType::ChannelPressure:
        return fluid_synth_channel_pressure(
            impl_->synth.get(), channel, message.pressure
        );
    case MidiMessageType::PitchBend:
        return fluid_synth_pitch_bend(impl_->synth.get(), channel, message.pitch);
    case MidiMessageType::MachineControl:
    case MidiMessageType::Other:
        return FLUID_OK;
    }
    return FLUID_OK;
}

int SynthEngine::noteOn(int channel, int key, int velocity) {
    return fluid_synth_noteon(impl_->synth.get(), channel, key, velocity);
}

int SynthEngine::noteOff(int channel, int key) {
    return fluid_synth_noteoff(impl_->synth.get(), channel, key);
}

void SynthEngine::select(const SoundFontDefinition& soundfont, int channel) {
    const int soundfont_id = impl_->soundfont_ids.at(soundfont.id);
    const int result = fluid_synth_program_select(
        impl_->synth.get(),
        channel,
        soundfont_id,
        soundfont.bank,
        soundfont.preset
    );

    #ifdef ZETA_MIDI_TRACE
    std::osyncstream{std::cerr}
        << "[program_select]"
        << " path=" << soundfont.file
        << " sfid=" << soundfont_id
        << " channel=" << channel
        << " bank=" << soundfont.bank
        << " preset=" << soundfont.preset
        << " rc=" << result
        << '\n';
    #endif

    if (result != FLUID_OK) {
        throw std::runtime_error(
            "Could not select preset bank=" + std::to_string(soundfont.bank)
            + " preset=" + std::to_string(soundfont.preset)
            + " in SoundFont: " + soundfont.file.string()
        );
    }
}

void SynthEngine::allNotesOff() {
    for (int channel = 0; channel < impl_->midi_channel_count; ++channel) {
        allNotesOff(channel);
    }
}

void SynthEngine::allNotesOff(int channel) {
    fluid_synth_cc(impl_->synth.get(), channel, 64, 0);
    fluid_synth_cc(impl_->synth.get(), channel, 123, 0);
}

} // namespace zeta
