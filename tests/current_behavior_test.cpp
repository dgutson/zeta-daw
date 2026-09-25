#include "fake_fluidsynth.hpp"
#include "fake_midi_input.hpp"
#include "integration_support.hpp"
#include "../application.hpp"
#include "../synth_engine.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <hegel/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace integration_support;
using fake_fluidsynth::Call;
using fake_fluidsynth::CallKind;
using zeta::Application;
using zeta::MidiEvent;
using zeta::MidiInput;
using zeta::MidiMessageType;
using zeta::SynthEngine;

class CapturingOutputBuffer final : public std::streambuf {
public:
    bool waitFor(std::string_view text, std::chrono::milliseconds timeout = 1s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            return contents_.find(text) != std::string::npos;
        });
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
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string contents_;
};

class InteractiveSession final {
public:
    explicit InteractiveSession(Application& application)
        : application_(application),
          old_output_(std::cout.rdbuf(&output_)),
          thread_([&application] {
              application.run();
          }) {}

    ~InteractiveSession() {
        application_.shutdownRequested();
        if (thread_.joinable()) {
            thread_.join();
        }
        std::cout.rdbuf(old_output_);
    }

    InteractiveSession(const InteractiveSession&) = delete;
    InteractiveSession& operator=(const InteractiveSession&) = delete;

    bool waitForOutput(std::string_view text) {
        return output_.waitFor(text);
    }

    void join() {
        thread_.join();
    }

private:
    Application& application_;
    CapturingOutputBuffer output_;
    std::streambuf* old_output_;
    std::jthread thread_;
};

int pressLoopSlotControl() {
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

int pressSoundFontByNoteControl() {
    return fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::MachineControl),
        .machine_control_command = 0x09,
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

int emitNoteOn(int key, int velocity = 100, int channel = 0) {
    return fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = channel,
        .key = key,
        .velocity = velocity,
    });
}

int emitNoteOff(int key, int channel = 0) {
    return fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::NoteOff),
        .channel = channel,
        .key = key,
    });
}

void selectSlot(int key, int channel = 0) {
    ASSERT_EQ(pressLoopSlotControl(), 0);
    ASSERT_EQ(emitNoteOn(key, 100, channel), 0);
}

void completeTake(
    int slot_key,
    int note,
    std::chrono::milliseconds recording_time = 12ms
) {
    selectSlot(slot_key);
    ASSERT_EQ(emitNoteOn(note), 0);
    std::this_thread::sleep_for(recording_time / 2);
    ASSERT_EQ(emitNoteOff(note), 0);
    std::this_thread::sleep_for(recording_time / 2);
    ASSERT_EQ(pressLoopSlotControl(), 0);
}

bool hasCall(
    CallKind kind,
    int channel,
    std::optional<int> key = std::nullopt
) {
    return std::ranges::any_of(
        fake_fluidsynth::calls(),
        [&](const Call& call) {
            return call.kind == kind
                && call.channel == channel
                && (!key || call.key == *key);
        }
    );
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
    void start(
        std::vector<zeta::MidiControlChangeMapping>,
        Handler handler
    ) override {
        handler(MidiEvent{
            .type = MidiMessageType::NoteOn,
            .message = {
                .raw_type = raw(MidiMessageType::NoteOn),
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

    EXPECT_FALSE(hasCall(CallKind::SynthNoteOn, 0, 67));
}

TEST_F(CurrentBehaviorTest, ConfiguresChannelsBeforeSynthAndLoadsSoundFonts) {
    {
        Application application{testConfig(), fake_midi_input::makeInput()};
        const auto calls = fake_fluidsynth::calls();
        ASSERT_FALSE(calls.empty());
        const auto channels = std::ranges::find(
            calls,
            CallKind::ConfigureMidiChannels,
            &Call::kind
        );
        ASSERT_NE(channels, calls.end());
        EXPECT_EQ(channels->value, 16);
        EXPECT_EQ(
            std::ranges::count(
                calls,
                CallKind::ConfigureStringSetting,
                &Call::kind
            ),
            0
        );
        const auto gain = std::ranges::find(
            calls,
            CallKind::ConfigureNumberSetting,
            &Call::kind
        );
        ASSERT_NE(gain, calls.end());
        EXPECT_EQ(gain->text, "synth.gain");
        EXPECT_DOUBLE_EQ(gain->number_value, 0.5);
        const auto period_size = std::ranges::find(
            calls,
            std::string{"audio.period-size"},
            &Call::text
        );
        EXPECT_EQ(period_size, calls.end());
        const auto periods = std::ranges::find(
            calls,
            std::string{"audio.periods"},
            &Call::text
        );
        EXPECT_EQ(periods, calls.end());
        EXPECT_EQ(
            std::ranges::count(calls, CallKind::LoadSoundFont, &Call::kind),
            2
        );
        EXPECT_TRUE(hasCall(CallKind::SelectProgram, 0));
    }

    const auto calls = fake_fluidsynth::calls();
    const auto audio = std::ranges::find(calls, CallKind::DeleteAudioDriver, &Call::kind);
    const auto synth = std::ranges::find(calls, CallKind::DeleteSynth, &Call::kind);
    const auto settings = std::ranges::find(calls, CallKind::DeleteSettings, &Call::kind);
    EXPECT_LT(audio, synth);
    EXPECT_LT(synth, settings);
}

TEST_F(CurrentBehaviorTest, ConfiguresFluidSynthAudioOverrides) {
    auto config = testConfig();
    config.audio = {
        .driver = "alsa",
        .alsa_device = "plughw:3",
        .gain = 1.0,
        .period_size = 512,
        .periods = 4,
    };

    SynthEngine synth_engine{config};
    const auto calls = fake_fluidsynth::calls();

    const auto driver = std::ranges::find(
        calls,
        std::string{"audio.driver=alsa"},
        &Call::text
    );
    ASSERT_NE(driver, calls.end());
    EXPECT_EQ(driver->kind, CallKind::ConfigureStringSetting);

    const auto device = std::ranges::find(
        calls,
        std::string{"audio.alsa.device=plughw:3"},
        &Call::text
    );
    ASSERT_NE(device, calls.end());
    EXPECT_EQ(device->kind, CallKind::ConfigureStringSetting);

    const auto gain = std::ranges::find(
        calls,
        CallKind::ConfigureNumberSetting,
        &Call::kind
    );
    ASSERT_NE(gain, calls.end());
    EXPECT_EQ(gain->text, "synth.gain");
    EXPECT_DOUBLE_EQ(gain->number_value, 1.0);

    const auto period_size = std::ranges::find(
        calls,
        std::string{"audio.period-size"},
        &Call::text
    );
    ASSERT_NE(period_size, calls.end());
    EXPECT_EQ(period_size->kind, CallKind::ConfigureIntegerSetting);
    EXPECT_EQ(period_size->value, 512);

    const auto periods = std::ranges::find(
        calls,
        std::string{"audio.periods"},
        &Call::text
    );
    ASSERT_NE(periods, calls.end());
    EXPECT_EQ(periods->kind, CallKind::ConfigureIntegerSetting);
    EXPECT_EQ(periods->value, 4);
}

TEST_F(CurrentBehaviorTest, ReadyRoutesLiveMidiToChannelZero) {
    Application application{testConfig(), fake_midi_input::makeInput()};

    ASSERT_EQ(emitNoteOn(64, 91, 7), 0);
    EXPECT_TRUE(hasCall(CallKind::SynthNoteOn, 0, 64));
    EXPECT_FALSE(hasCall(CallKind::SynthNoteOn, 7, 64));
}

TEST_F(CurrentBehaviorTest, SelectionNoteIsRawChannelIndependentAndConsumed) {
    Application application{testConfig(), fake_midi_input::makeInput()};

    ASSERT_EQ(pressOctaveUpControl(), 0);
    selectSlot(first_slot_key, 15);

    EXPECT_FALSE(hasCall(CallKind::SynthNoteOn, 0, first_slot_key));
    EXPECT_FALSE(hasCall(CallKind::SynthNoteOn, first_slot_channel, first_slot_key));
    ASSERT_EQ(pressLoopSlotControl(), 0);
}

TEST_F(CurrentBehaviorTest, ButtonOnlyCompletionPlaysFirstNoteAtOffsetZero) {
    Application application{testConfig(), fake_midi_input::makeInput()};

    selectSlot(first_slot_key);
    std::this_thread::sleep_for(30ms);
    ASSERT_EQ(emitNoteOn(60), 0);
    ASSERT_EQ(emitNoteOff(60), 0);
    std::this_thread::sleep_for(100ms);
    ASSERT_EQ(pressLoopSlotControl(), 0);

    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2, 25ms));
    selectSlot(first_slot_key);
}

TEST_F(CurrentBehaviorTest, RegularSlotCannotArmBeforeGuideIsLooping) {
    Application application{testConfig(), fake_midi_input::makeInput()};

    selectSlot(second_slot_key);
    ASSERT_EQ(emitNoteOn(64), 0);

    EXPECT_TRUE(hasCall(CallKind::SynthNoteOn, 0, 64));
    EXPECT_FALSE(hasCall(CallKind::SynthNoteOn, second_slot_channel, 64));
}

TEST_F(CurrentBehaviorTest, TwoSlotsLoopConcurrently) {
    Application application{testConfig(), fake_midi_input::makeInput()};

    completeTake(first_slot_key, 60);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));
    completeTake(second_slot_key, 64);

    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 3));
    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 2));
}

TEST_F(CurrentBehaviorTest, ExistingLoopContinuesWhileAnotherSlotIsArmedAndRecorded) {
    Application application{testConfig(), fake_midi_input::makeInput()};

    completeTake(first_slot_key, 60, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));
    const auto before_arm = callCount(CallKind::SynthNoteOn, first_slot_channel, 60);

    selectSlot(second_slot_key);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, before_arm + 1));
    const auto before_recording = callCount(
        CallKind::SynthNoteOn,
        first_slot_channel,
        60
    );
    ASSERT_EQ(emitNoteOn(64), 0);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, before_recording + 1));
    ASSERT_EQ(emitNoteOff(64), 0);
    ASSERT_EQ(pressLoopSlotControl(), 0);
    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 2));
}

TEST_F(CurrentBehaviorTest, StoppingRegularSlotDoesNotAffectGuideOrPeer) {
    Application application{threeSlotConfig(), fake_midi_input::makeInput()};
    completeTake(first_slot_key, 60, 10ms);
    completeTake(second_slot_key, 64, 10ms);
    completeTake(third_slot_key, 67, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));
    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 2));
    ASSERT_TRUE(waitForNoteCount(third_slot_channel, 67, 2));

    selectSlot(second_slot_key);
    const auto stopped_count = callCount(
        CallKind::SynthNoteOn, second_slot_channel, 64
    );
    const auto guide_count = callCount(
        CallKind::SynthNoteOn, first_slot_channel, 60
    );
    const auto peer_count = callCount(
        CallKind::SynthNoteOn, third_slot_channel, 67
    );

    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, guide_count + 2));
    ASSERT_TRUE(waitForNoteCount(third_slot_channel, 67, peer_count + 2));
    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, second_slot_channel, 64),
        stopped_count
    );

    selectSlot(second_slot_key);
    ASSERT_EQ(emitNoteOn(69), 0);
    ASSERT_EQ(emitNoteOff(69), 0);
    std::this_thread::sleep_for(2ms);
    ASSERT_EQ(pressLoopSlotControl(), 0);

    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 69, 2));
    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, second_slot_channel, 64),
        stopped_count
    );
}

TEST_F(CurrentBehaviorTest, StoppingGuideStopsAndDiscardsEveryRegularSlot) {
    Application application{threeSlotConfig(), fake_midi_input::makeInput()};
    completeTake(first_slot_key, 60, 10ms);
    completeTake(second_slot_key, 64, 10ms);
    completeTake(third_slot_key, 67, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));
    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 2));
    ASSERT_TRUE(waitForNoteCount(third_slot_channel, 67, 2));

    selectSlot(first_slot_key);
    const auto guide_count = callCount(
        CallKind::SynthNoteOn, first_slot_channel, 60
    );
    const auto second_count = callCount(
        CallKind::SynthNoteOn, second_slot_channel, 64
    );
    const auto third_count = callCount(
        CallKind::SynthNoteOn, third_slot_channel, 67
    );
    std::this_thread::sleep_for(40ms);

    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, first_slot_channel, 60),
        guide_count
    );
    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, second_slot_channel, 64),
        second_count
    );
    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, third_slot_channel, 67),
        third_count
    );

    selectSlot(second_slot_key);
    ASSERT_EQ(emitNoteOn(69), 0);
    EXPECT_TRUE(hasCall(CallKind::SynthNoteOn, 0, 69));
    EXPECT_FALSE(hasCall(CallKind::SynthNoteOn, second_slot_channel, 69));
}

TEST_F(CurrentBehaviorTest, DependentCompletionEndsHeldNoteAndRecordsItsRelease) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    completeTake(first_slot_key, 60, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));

    selectSlot(second_slot_key);
    ASSERT_EQ(emitNoteOn(64), 0);
    const auto silence_count = controlChangeCount(second_slot_channel, 123);
    std::this_thread::sleep_for(8ms);
    ASSERT_EQ(pressLoopSlotControl(), 0);

    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 2));
    ASSERT_TRUE(waitForNoteOffCount(second_slot_channel, 64, 1));
    EXPECT_GT(controlChangeCount(second_slot_channel, 123), silence_count);
}

TEST_F(CurrentBehaviorTest, ImmediateStopThenRearmAcceptsReplacement) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    completeTake(first_slot_key, 60, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));

    selectSlot(first_slot_key);
    selectSlot(first_slot_key);
    ASSERT_EQ(emitNoteOn(67), 0);
    ASSERT_EQ(emitNoteOff(67), 0);
    std::this_thread::sleep_for(2ms);
    ASSERT_EQ(pressLoopSlotControl(), 0);

    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 67, 2));
}

TEST_F(CurrentBehaviorTest, ReplacementNeverResumesOldTake) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    completeTake(first_slot_key, 60, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));

    selectSlot(first_slot_key);
    const auto old_count = callCount(CallKind::SynthNoteOn, first_slot_channel, 60);
    completeTake(first_slot_key, 67, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 67, 3));

    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, first_slot_channel, 60),
        old_count
    );
}

TEST_F(CurrentBehaviorTest, CancelingReplacementLeavesNoResumableTake) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    completeTake(first_slot_key, 60, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));

    selectSlot(first_slot_key);
    const auto old_count = callCount(CallKind::SynthNoteOn, first_slot_channel, 60);
    selectSlot(first_slot_key);
    ASSERT_EQ(pressLoopSlotControl(), 0);
    std::this_thread::sleep_for(30ms);

    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, first_slot_channel, 60),
        old_count
    );
    selectSlot(first_slot_key);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, first_slot_channel, 60),
        old_count
    );
    ASSERT_EQ(pressLoopSlotControl(), 0);
}

TEST_F(CurrentBehaviorTest, SoundFontAndOctaveSnapshotsAreIndependentPerSlot) {
    Application application{testConfig(), fake_midi_input::makeInput()};

    ASSERT_EQ(pressNextSoundFontControl(), 0);
    ASSERT_EQ(pressOctaveUpControl(), 0);
    selectSlot(first_slot_key);
    ASSERT_EQ(pressNextSoundFontControl(), 0);
    ASSERT_EQ(emitNoteOn(48), 0);
    ASSERT_EQ(emitNoteOff(48), 0);
    std::this_thread::sleep_for(2ms);
    ASSERT_EQ(pressLoopSlotControl(), 0);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));

    ASSERT_EQ(pressSoundFontByNoteControl(), 0);
    ASSERT_EQ(emitNoteOn(72), 0);
    EXPECT_TRUE(std::ranges::any_of(
        fake_fluidsynth::calls(),
        [](const Call& call) {
            return call.kind == CallKind::SelectProgram
                && call.channel == 0
                && call.soundfont_id == 2;
        }
    ));
    EXPECT_TRUE(std::ranges::any_of(
        fake_fluidsynth::calls(),
        [](const Call& call) {
            return call.kind == CallKind::SelectProgram
                && call.channel == first_slot_channel
                && call.soundfont_id == 1;
        }
    ));
}

TEST_F(CurrentBehaviorTest, RecordingRestoresLockedProgramBeforePlayback) {
    Application application{testConfig(), fake_midi_input::makeInput()};

    selectSlot(first_slot_key);
    ASSERT_EQ(emitNoteOn(64), 0);
    ASSERT_EQ(fake_midi_input::emitMidi({
        .type = raw(MidiMessageType::ProgramChange),
        .channel = 0,
        .program = 99,
    }), 0);

    const auto locked_program_selections = [] {
        return std::ranges::count_if(
            fake_fluidsynth::calls(),
            [](const Call& call) {
                return call.kind == CallKind::SelectProgram
                    && call.channel == first_slot_channel
                    && call.soundfont_id == 1
                    && call.bank == 0
                    && call.preset == 0;
            }
        );
    };
    const auto before_completion = locked_program_selections();

    std::this_thread::sleep_for(2ms);
    ASSERT_EQ(pressLoopSlotControl(), 0);

    EXPECT_EQ(locked_program_selections(), before_completion + 1);
}

TEST_F(CurrentBehaviorTest, LivePlayingRemainsAvailableWhileSlotsLoop) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    completeTake(first_slot_key, 60, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));

    ASSERT_EQ(emitNoteOn(74, 80, 9), 0);
    EXPECT_TRUE(hasCall(CallKind::SynthNoteOn, 0, 74));
}

TEST_F(CurrentBehaviorTest, ShutdownJoinsEveryWorkerAndSilencesEveryChannel) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    InteractiveSession session{application};
    ASSERT_TRUE(session.waitForOutput("MIDI looper ready."));
    completeTake(first_slot_key, 60, 10ms);
    completeTake(second_slot_key, 64, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));
    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 2));

    const std::array silence_counts_before{
        controlChangeCount(0, 123),
        controlChangeCount(first_slot_channel, 123),
        controlChangeCount(second_slot_channel, 123),
    };

    application.shutdownRequested();
    session.join();
    const auto first_count = callCount(
        CallKind::SynthNoteOn,
        first_slot_channel,
        60
    );
    const auto second_count = callCount(
        CallKind::SynthNoteOn,
        second_slot_channel,
        64
    );
    std::this_thread::sleep_for(30ms);

    EXPECT_EQ(callCount(CallKind::SynthNoteOn, first_slot_channel, 60), first_count);
    EXPECT_EQ(callCount(CallKind::SynthNoteOn, second_slot_channel, 64), second_count);
    const std::array channels{0, first_slot_channel, second_slot_channel};
    for (std::size_t index = 0; index < channels.size(); ++index) {
        EXPECT_GT(
            controlChangeCount(channels[index], 123),
            silence_counts_before[index]
        );
    }
}

namespace gs = hegel::generators;

constexpr int live_channel = 0;
constexpr int midi_channel_count = 16;
const std::vector<int> plain_note_keys{60, 64};
constexpr int minimum_concurrent_performers = 1;
constexpr int maximum_concurrent_performers = 3;
// Shutdown usually comes within the first rounds; later rounds only prove
// that stimuli stay inert, so a short run keeps the suite fast.
constexpr std::int64_t performance_rounds = 10;
constexpr int longest_pause_microseconds = 3000;
constexpr int shortest_loop_note_microseconds = 500;
constexpr int longest_loop_note_microseconds = 4000;
constexpr int longest_trailing_silence_microseconds = 2000;

class ConcurrentPerformanceMachine final
    : public hegel::stateful::ConcurrentStateMachine<
        ConcurrentPerformanceMachine
    > {
public:
    ConcurrentPerformanceMachine()
        : application_{testConfig(), fake_midi_input::makeInput()} {
        const auto config = testConfig();
        last_channel_ = static_cast<int>(config.loop_slots.size());
        for (const auto& slot : config.loop_slots) {
            slot_keys_.push_back(slot.key);
        }
        performance_keys_ = slot_keys_;
        for (const auto& soundfont : config.soundfonts) {
            if (soundfont.key) {
                performance_keys_.push_back(*soundfont.key);
            }
        }
        performance_keys_.insert(
            performance_keys_.end(),
            plain_note_keys.begin(),
            plain_note_keys.end()
        );
    }

    std::vector<hegel::stateful::ConcurrentRule<ConcurrentPerformanceMachine>>
    rules() {
        using Rule =
            hegel::stateful::ConcurrentRule<ConcurrentPerformanceMachine>;
        // A single group lets every performer action race every other one,
        // shutdown included.
        const std::string group = "performance";

        return {
            Rule("press key", group, [](hegel::TestCase& tc, auto& machine) {
                const int key = machine.drawPerformanceKey(tc);
                const int channel = drawInputChannel(tc);
                emitNoteOn(key, 100, channel);
            }),
            Rule("release key", group, [](hegel::TestCase& tc, auto& machine) {
                const int key = machine.drawPerformanceKey(tc);
                const int channel = drawInputChannel(tc);
                emitNoteOff(key, channel);
            }),
            Rule("press loop-slot control", group, [](hegel::TestCase&, auto&) {
                pressLoopSlotControl();
            }),
            Rule("press next SoundFont", group, [](hegel::TestCase&, auto&) {
                pressNextSoundFontControl();
            }),
            Rule("press SoundFont by note", group, [](hegel::TestCase&, auto&) {
                pressSoundFontByNoteControl();
            }),
            Rule("press octave down", group, [](hegel::TestCase&, auto&) {
                pressOctaveDownControl();
            }),
            Rule("press octave up", group, [](hegel::TestCase&, auto&) {
                pressOctaveUpControl();
            }),
            Rule("record a loop", group, [](hegel::TestCase& tc, auto& machine) {
                machine.recordLoop(tc);
            }),
            Rule("wait", group, [](hegel::TestCase& tc, auto&) {
                std::this_thread::sleep_for(
                    drawDuration(tc, "pause", 0, longest_pause_microseconds)
                );
            }),
            Rule("request shutdown", group, [](hegel::TestCase&, auto& machine) {
                machine.requestShutdown();
            }),
        };
    }

    std::vector<hegel::stateful::Invariant<ConcurrentPerformanceMachine>>
    invariants() const {
        using Invariant =
            hegel::stateful::Invariant<ConcurrentPerformanceMachine>;

        return {
            Invariant(
                "notes sound only on configured channels",
                [](const auto& machine) {
                    if (!machine.notesSoundOnlyOnConfiguredChannels()) {
                        throw std::runtime_error(
                            "a note sounded outside the configured channels"
                        );
                    }
                }
            ),
            Invariant(
                "nothing sounds after shutdown",
                [](const auto& machine) {
                    if (!machine.nothingSoundsAfterShutdown()) {
                        throw std::runtime_error(
                            "a note sounded after shutdown returned"
                        );
                    }
                }
            ),
            Invariant(
                "shutdown silences every channel",
                [](const auto& machine) {
                    if (!machine.shutdownSilencedEverySoundingChannel()) {
                        throw std::runtime_error(
                            "a channel was left sounding after shutdown"
                        );
                    }
                }
            ),
        };
    }

    void requestShutdown() {
        application_.shutdownRequested();
        const auto log_size = fake_fluidsynth::calls().size();

        std::lock_guard lock(shutdown_mutex_);
        shutdown_log_size_ = shutdown_log_size_
            ? std::min(*shutdown_log_size_, log_size)
            : log_size;
    }

    bool notesSoundOnlyOnConfiguredChannels() const {
        return std::ranges::all_of(
            fake_fluidsynth::calls(),
            [this](const Call& call) {
                return call.kind != CallKind::SynthNoteOn
                    || (call.channel >= live_channel
                        && call.channel <= last_channel_);
            }
        );
    }

    bool nothingSoundsAfterShutdown() const {
        const auto boundary = shutdownLogSize();
        if (!boundary) {
            return true;
        }

        const auto calls = fake_fluidsynth::calls();
        return std::none_of(
            calls.begin() + static_cast<std::ptrdiff_t>(*boundary),
            calls.end(),
            [](const Call& call) {
                return call.kind == CallKind::SynthNoteOn;
            }
        );
    }

    bool shutdownSilencedEverySoundingChannel() const {
        if (!shutdownLogSize()) {
            return true;
        }

        const auto calls = fake_fluidsynth::calls();
        for (int channel = live_channel; channel <= last_channel_; ++channel) {
            std::optional<std::size_t> last_note_on;
            std::optional<std::size_t> last_all_notes_off;
            for (std::size_t index = 0; index < calls.size(); ++index) {
                const auto& call = calls[index];
                if (call.channel != channel) {
                    continue;
                }
                if (call.kind == CallKind::SynthNoteOn) {
                    last_note_on = index;
                } else if (call.kind == CallKind::SynthControlChange
                    && call.control == all_notes_off_controller) {
                    last_all_notes_off = index;
                }
            }
            const bool silenced_after_last_note = !last_note_on
                || (last_all_notes_off && *last_all_notes_off > *last_note_on);
            if (!silenced_after_last_note) {
                return false;
            }
        }
        return true;
    }

private:
    static int drawInputChannel(hegel::TestCase& tc) {
        return tc.draw(
            "channel",
            gs::integers<int>({
                .min_value = 0,
                .max_value = midi_channel_count - 1,
            }),
            repeatable
        );
    }

    int drawPerformanceKey(hegel::TestCase& tc) const {
        return tc.draw("key", gs::sampled_from(performance_keys_), repeatable);
    }

    void recordLoop(hegel::TestCase& tc) const {
        const int slot_key = tc.draw(
            "slot_key",
            gs::sampled_from(slot_keys_),
            repeatable
        );
        const int loop_key = tc.draw(
            "loop_key",
            gs::sampled_from(plain_note_keys),
            repeatable
        );
        const auto held_for = drawDuration(
            tc,
            "held_for",
            shortest_loop_note_microseconds,
            longest_loop_note_microseconds
        );
        const auto trailing_silence = drawDuration(
            tc,
            "trailing_silence",
            0,
            longest_trailing_silence_microseconds
        );

        pressLoopSlotControl();
        emitNoteOn(slot_key);
        emitNoteOn(loop_key);
        std::this_thread::sleep_for(held_for);
        emitNoteOff(loop_key);
        std::this_thread::sleep_for(trailing_silence);
        pressLoopSlotControl();
    }

    std::optional<std::size_t> shutdownLogSize() const {
        std::lock_guard lock(shutdown_mutex_);
        return shutdown_log_size_;
    }

    // Application announces every performer action on std::cout; discarding
    // it keeps a failing run's report, which Hegel writes to stderr, readable.
    DiscardedStandardOutput discarded_output_;
    Application application_;
    int last_channel_{};
    std::vector<int> slot_keys_;
    std::vector<int> performance_keys_;
    mutable std::mutex shutdown_mutex_;
    std::optional<std::size_t> shutdown_log_size_;
};

TEST(CurrentBehaviorPropertyTest, ShutdownRacingMidiAndLoopsLeavesNothingSounding) {
    hegel::Settings settings;
    settings.stateful_step_count = performance_rounds;

    hegel::test([](hegel::TestCase& tc) {
        fake_fluidsynth::reset();
        fake_midi_input::reset();
        ConcurrentPerformanceMachine machine;

        hegel::stateful::run_concurrent(
            machine,
            tc,
            minimum_concurrent_performers,
            maximum_concurrent_performers
        );

        machine.requestShutdown();
        ASSERT_TRUE(machine.nothingSoundsAfterShutdown());
        ASSERT_TRUE(machine.shutdownSilencedEverySoundingChannel());
    }, settings);
}

} // namespace
