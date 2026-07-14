#include "fake_fluidsynth.hpp"
#include "fake_midi_input.hpp"
#include "../application.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <streambuf>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using fake_fluidsynth::Call;
using fake_fluidsynth::CallKind;
using zeta::Application;
using zeta::ApplicationConfig;
using zeta::MidiControlBinding;
using zeta::MidiControlType;
using zeta::MidiEvent;
using zeta::MidiInput;
using zeta::MidiMessageType;
using zeta::SoundFontDefinition;

constexpr int raw(MidiMessageType type) {
    return static_cast<int>(type);
}

class CapturingOutputBuffer final : public std::streambuf {
public:
    bool waitFor(std::string_view text, std::chrono::milliseconds timeout = 1s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            return contents_.find(text) != std::string::npos;
        });
    }

    std::string contents() const {
        std::lock_guard lock(mutex_);
        return contents_;
    }

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        {
            std::lock_guard lock(mutex_);
            contents_.append(data, static_cast<std::size_t>(size));
        }
        changed_.notify_all();
        return size;
    }

    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }

        {
            std::lock_guard lock(mutex_);
            contents_.push_back(traits_type::to_char_type(character));
        }
        changed_.notify_all();
        return character;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string contents_;
};

class InteractiveSession final {
public:
    explicit InteractiveSession(Application& application)
        : application_(application),
          old_output_(std::cout.rdbuf(&output_)),
          old_error_(std::cerr.rdbuf(&error_)),
          thread_([&application] {
              application.run();
          }) {}

    ~InteractiveSession() {
        application_.shutdownRequested();
        if (thread_.joinable()) {
            thread_.join();
        }

        std::cout.rdbuf(old_output_);
        std::cerr.rdbuf(old_error_);
    }

    InteractiveSession(const InteractiveSession&) = delete;
    InteractiveSession& operator=(const InteractiveSession&) = delete;

    bool waitForOutput(std::string_view text) {
        return output_.waitFor(text);
    }

    void join() {
        thread_.join();
    }

    std::string output() const {
        return output_.contents();
    }

private:
    Application& application_;
    CapturingOutputBuffer output_;
    CapturingOutputBuffer error_;
    std::streambuf* old_output_;
    std::streambuf* old_error_;
    std::jthread thread_;
};

ApplicationConfig testConfig() {
    return {
        .soundfonts = {
            SoundFontDefinition{
                .id = "loop",
                .file = "loop.sf2",
                .bank = 0,
                .preset = 34,
            },
            SoundFontDefinition{
                .id = "live",
                .file = "live.sf2",
                .bank = 0,
                .preset = 0,
            },
        },
        .recording_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x05,
        },
        .next_soundfont_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x01,
        },
        .octave_down_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x02,
        },
        .octave_up_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x06,
        },
    };
}

int pressRecordingControl() {
    return fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::MachineControl),
        .machine_control_command = 0x05,
    });
}

int pressNextSoundFontControl() {
    return fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::MachineControl),
        .machine_control_command = 0x01,
    });
}

int pressOctaveDownControl() {
    return fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::MachineControl),
        .machine_control_command = 0x02,
    });
}

int pressOctaveUpControl() {
    return fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::MachineControl),
        .machine_control_command = 0x06,
    });
}

bool hasCall(
    const std::vector<Call>& calls,
    CallKind kind,
    std::optional<int> channel = std::nullopt,
    std::optional<int> key = std::nullopt,
    std::optional<int> value = std::nullopt
) {
    return std::ranges::any_of(calls, [&](const Call& call) {
        return call.kind == kind
            && (!channel || call.channel == *channel)
            && (!key || call.key == *key)
            && (!value || call.value == *value);
    });
}

std::size_t firstIndexOf(const std::vector<Call>& calls, CallKind kind) {
    const auto found = std::ranges::find(calls, kind, &Call::kind);
    return static_cast<std::size_t>(std::distance(calls.begin(), found));
}

class CurrentBehaviorTest : public ::testing::Test {
protected:
    void SetUp() override {
        fake_fluidsynth::reset();
        fake_midi_input::reset();
    }
};

class StartupNoteMidiInput final : public MidiInput {
public:
    void start(Handler handler) override {
        handler(MidiEvent{
            .type = MidiMessageType::NoteOn,
            .message = {
                .raw_type = raw(MidiMessageType::NoteOn),
                .channel = 0,
                .key = 67,
                .velocity = 100,
            },
        });
    }

    void stop() noexcept override {}
};

TEST_F(CurrentBehaviorTest, IgnoresMidiDeliveredWhileInputIsStarting) {
    Application application{
        testConfig(),
        std::make_unique<StartupNoteMidiInput>()
    };

    const auto calls = fake_fluidsynth::calls();
    EXPECT_FALSE(hasCall(calls, CallKind::SynthNoteOn, 0, 67));
}

TEST_F(CurrentBehaviorTest, LoadsInitialSoundFontOnBothChannels) {
    {
        Application application{testConfig(), fake_midi_input::makeInput()};

        const auto calls = fake_fluidsynth::calls();
        ASSERT_GE(calls.size(), 4U);

        EXPECT_EQ(calls[0].kind, CallKind::LoadSoundFont);
        EXPECT_EQ(calls[0].text, "loop.sf2");
        EXPECT_EQ(calls[1].kind, CallKind::LoadSoundFont);
        EXPECT_EQ(calls[1].text, "live.sf2");

        EXPECT_EQ(calls[2].kind, CallKind::SelectProgram);
        EXPECT_EQ(calls[2].channel, 1);
        EXPECT_EQ(calls[2].bank, 0);
        EXPECT_EQ(calls[2].preset, 34);

        EXPECT_EQ(calls[3].kind, CallKind::SelectProgram);
        EXPECT_EQ(calls[3].channel, 0);
        EXPECT_EQ(calls[3].bank, 0);
        EXPECT_EQ(calls[3].preset, 34);
    }

    const auto calls = fake_fluidsynth::calls();
    const auto audio_deleted = firstIndexOf(calls, CallKind::DeleteAudioDriver);
    const auto synth_deleted = firstIndexOf(calls, CallKind::DeleteSynth);
    const auto settings_deleted = firstIndexOf(calls, CallKind::DeleteSettings);

    EXPECT_LT(audio_deleted, synth_deleted);
    EXPECT_LT(synth_deleted, settings_deleted);
}

TEST_F(CurrentBehaviorTest, EagerlyLoadsEveryUniqueConfiguredSoundFont) {
    auto config = testConfig();
    config.soundfonts.push_back({
        .id = "alternate-live-preset",
        .file = "live.sf2",
        .bank = 0,
        .preset = 8,
    });
    config.soundfonts.push_back({
        .id = "organ",
        .file = "organ.sf2",
        .bank = 0,
        .preset = 16,
    });

    Application application{
        std::move(config),
        fake_midi_input::makeInput()
    };

    const auto calls = fake_fluidsynth::calls();
    EXPECT_EQ(
        std::ranges::count(calls, CallKind::LoadSoundFont, &Call::kind),
        3
    );
    EXPECT_TRUE(std::ranges::any_of(calls, [](const Call& call) {
        return call.kind == CallKind::LoadSoundFont && call.text == "organ.sf2";
    }));
}

TEST_F(CurrentBehaviorTest, RecordingControlIsConsumedBeforeSynthRouting) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    InteractiveSession session{application};

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("Recording..."));

    const auto calls = fake_fluidsynth::calls();
    EXPECT_FALSE(std::ranges::any_of(calls, [](const Call& call) {
        return call.kind == CallKind::HandleMidi
            && call.type == raw(MidiMessageType::MachineControl);
    }));

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    session.join();
}

TEST_F(CurrentBehaviorTest, ShutdownRequestWakesTheApplicationRunLoop) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    InteractiveSession session{application};
    ASSERT_TRUE(session.waitForOutput("MIDI looper ready."));

    application.shutdownRequested();
    session.join();
}

TEST_F(CurrentBehaviorTest, ReadyRoutesMidiToDedicatedLiveChannel) {
    Application application{testConfig(), fake_midi_input::makeInput()};

    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 7,
        .key = 64,
        .velocity = 91,
    }), FLUID_OK);

    const auto calls = fake_fluidsynth::calls();
    EXPECT_TRUE(hasCall(calls, CallKind::HandleMidi, 0, 64));
    EXPECT_FALSE(hasCall(calls, CallKind::HandleMidi, 7, 64));
    EXPECT_TRUE(hasCall(calls, CallKind::SynthNoteOn, 0, 64));
}

TEST_F(CurrentBehaviorTest, SuppressesZeroExpressionBeforeItReachesTheSynth) {
    Application application{testConfig(), fake_midi_input::makeInput()};

    const auto before = fake_fluidsynth::calls();
    const auto handled_before = std::ranges::count(before, CallKind::HandleMidi, &Call::kind);

    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::ControlChange),
        .channel = 1,
        .control = 11,
        .value = 0,
    }), FLUID_OK);

    const auto after_suppressed = fake_fluidsynth::calls();
    EXPECT_EQ(
        std::ranges::count(after_suppressed, CallKind::HandleMidi, &Call::kind),
        handled_before
    );

    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::ControlChange),
        .channel = 0,
        .control = 11,
        .value = 1,
    }), FLUID_OK);

    const auto after_forwarded = fake_fluidsynth::calls();
    EXPECT_TRUE(hasCall(after_forwarded, CallKind::HandleMidi, 0, std::nullopt, 1));
}

TEST_F(CurrentBehaviorTest, ArmedMonitorsOnLoopChannelButDoesNotStartOnNoteOff) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    InteractiveSession session{application};

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("Recording..."));

    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOff),
        .channel = 0,
        .key = 60,
    }), FLUID_OK);
    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 0,
        .key = 60,
        .velocity = 0,
    }), FLUID_OK);
    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::ControlChange),
        .channel = 0,
        .control = 7,
        .value = 99,
    }), FLUID_OK);

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    session.join();

    const auto calls = fake_fluidsynth::calls();
    EXPECT_TRUE(hasCall(calls, CallKind::HandleMidi, 1, 60));
    EXPECT_TRUE(hasCall(calls, CallKind::HandleMidi, 1, std::nullopt, 99));
    EXPECT_THAT(session.output(), ::testing::HasSubstr("No notes were recorded. Exiting."));
}

TEST_F(CurrentBehaviorTest, FirstNoteStartsRecordingAndCompletedTakeLoopsOnChannelOne) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    InteractiveSession session{application};

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("Recording..."));

    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 0,
        .key = 60,
        .velocity = 100,
    }), FLUID_OK);

    std::this_thread::sleep_for(15ms);

    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOff),
        .channel = 0,
        .key = 60,
    }), FLUID_OK);

    std::this_thread::sleep_for(10ms);
    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("Looping."));

    ASSERT_TRUE(fake_fluidsynth::waitUntil([](const std::vector<Call>& calls) {
        return hasCall(calls, CallKind::SynthNoteOn, 1, 60)
            && hasCall(calls, CallKind::SynthNoteOff, 1, 60);
    }));

    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 1,
        .key = 72,
        .velocity = 80,
    }), FLUID_OK);
    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::PitchBend),
        .channel = 1,
        .pitch = 16383,
    }), FLUID_OK);

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    session.join();

    const auto calls = fake_fluidsynth::calls();
    EXPECT_TRUE(hasCall(calls, CallKind::HandleMidi, 1, 60));
    EXPECT_TRUE(hasCall(calls, CallKind::SynthNoteOn, 1, 60));
    EXPECT_TRUE(hasCall(calls, CallKind::SynthNoteOff, 1, 60));
    EXPECT_TRUE(hasCall(calls, CallKind::HandleMidi, 0, 72));
    EXPECT_FALSE(hasCall(calls, CallKind::HandleMidi, 1, 72));
    EXPECT_TRUE(std::ranges::any_of(calls, [](const Call& call) {
        return call.kind == CallKind::HandleMidi
            && call.type == raw(MidiMessageType::PitchBend)
            && call.channel == 0;
    }));
    EXPECT_FALSE(std::ranges::any_of(calls, [](const Call& call) {
        return call.kind == CallKind::HandleMidi
            && call.type == raw(MidiMessageType::PitchBend)
            && call.channel == 1;
    }));
    EXPECT_THAT(
        session.output(),
        ::testing::HasSubstr("Use the configured MIDI control to stop and quit.")
    );
}

TEST_F(CurrentBehaviorTest, StartingLoopPlaybackRestoresCurrentProgram) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    InteractiveSession session{application};

    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::ProgramChange),
        .channel = 0,
        .program = 99,
    }), FLUID_OK);

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("Recording..."));
    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 0,
        .key = 60,
        .velocity = 100,
    }), FLUID_OK);
    std::this_thread::sleep_for(2ms);
    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("Looping."));

    const auto calls = fake_fluidsynth::calls();
    EXPECT_EQ(std::ranges::count_if(calls, [](const Call& call) {
        return call.kind == CallKind::SelectProgram
            && call.channel == 0
            && call.soundfont_id == 1
            && call.bank == 0
            && call.preset == 34;
    }), 2);

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    session.join();
}

TEST_F(CurrentBehaviorTest, CyclesSoundFontsWithoutChangingActiveRecording) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    InteractiveSession session{application};

    ASSERT_EQ(pressNextSoundFontControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("SoundFont selected: live"));

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("Recording..."));

    ASSERT_EQ(pressNextSoundFontControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("SoundFont selected: loop"));

    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 0,
        .key = 60,
        .velocity = 100,
    }), FLUID_OK);

    const auto selections_before_ignored_press = std::ranges::count(
        fake_fluidsynth::calls(),
        CallKind::SelectProgram,
        &Call::kind
    );
    ASSERT_EQ(pressNextSoundFontControl(), FLUID_OK);
    EXPECT_EQ(
        std::ranges::count(
            fake_fluidsynth::calls(),
            CallKind::SelectProgram,
            &Call::kind
        ),
        selections_before_ignored_press
    );

    std::this_thread::sleep_for(2ms);
    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("Looping."));

    ASSERT_EQ(pressNextSoundFontControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("SoundFont selected: live"));

    const auto calls = fake_fluidsynth::calls();
    std::vector<std::pair<int, int>> selections;
    for (const auto& call : calls) {
        if (call.kind == CallKind::SelectProgram) {
            selections.emplace_back(call.channel, call.soundfont_id);
        }
    }
    EXPECT_THAT(selections, ::testing::ElementsAre(
        std::pair{1, 1},
        std::pair{0, 1},
        std::pair{0, 2},
        std::pair{1, 2},
        std::pair{1, 1},
        std::pair{0, 1},
        std::pair{0, 2}
    ));

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    session.join();
}

TEST_F(CurrentBehaviorTest, TransposesLivePlayingAndLocksTheRecordedLoop) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    InteractiveSession session{application};

    ASSERT_EQ(pressOctaveUpControl(), FLUID_OK);
    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 0,
        .key = 48,
        .velocity = 100,
    }), FLUID_OK);
    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOff),
        .channel = 0,
        .key = 48,
    }), FLUID_OK);

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("Recording..."));
    ASSERT_EQ(pressOctaveUpControl(), FLUID_OK);

    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 0,
        .key = 48,
        .velocity = 100,
    }), FLUID_OK);
    std::this_thread::sleep_for(2ms);
    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOff),
        .channel = 0,
        .key = 48,
    }), FLUID_OK);

    ASSERT_EQ(pressOctaveDownControl(), FLUID_OK);
    std::this_thread::sleep_for(2ms);
    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    ASSERT_TRUE(session.waitForOutput("Looping."));
    ASSERT_TRUE(fake_fluidsynth::waitUntil([](const std::vector<Call>& calls) {
        return hasCall(calls, CallKind::SynthNoteOn, 1, 72)
            && hasCall(calls, CallKind::SynthNoteOff, 1, 72);
    }));

    ASSERT_EQ(pressOctaveDownControl(), FLUID_OK);
    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 0,
        .key = 48,
        .velocity = 90,
    }), FLUID_OK);
    EXPECT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOff),
        .channel = 0,
        .key = 48,
    }), FLUID_OK);

    ASSERT_EQ(pressRecordingControl(), FLUID_OK);
    session.join();

    const auto calls = fake_fluidsynth::calls();
    EXPECT_TRUE(hasCall(calls, CallKind::SynthNoteOn, 0, 60));
    EXPECT_TRUE(hasCall(calls, CallKind::SynthNoteOff, 0, 60));
    EXPECT_TRUE(hasCall(calls, CallKind::SynthNoteOn, 1, 72));
    EXPECT_TRUE(hasCall(calls, CallKind::SynthNoteOff, 1, 72));
    EXPECT_FALSE(hasCall(calls, CallKind::SynthNoteOn, 1, 60));
}

} // namespace
