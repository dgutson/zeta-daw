#include "fake_fluidsynth.hpp"
#include "fake_midi_input.hpp"
#include "integration_support.hpp"
#include "../application.hpp"
#include "../loop_slot_group.hpp"
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
using zeta::ApplicationConfig;
using zeta::LoopSlotGroup;
using zeta::LoopSlotSelectionOutcome;
using zeta::LooperClock;
using zeta::Milliseconds;
using zeta::MidiEvent;
using zeta::MidiInput;
using zeta::MidiMessageType;
using zeta::OctaveTransposer;
using zeta::RecordedNoteKind;
using zeta::SlotId;
using zeta::SynthEngine;
using zeta::TakeTiming;

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

constexpr SlotId guide_slot = 0;
constexpr int midi_key_count = 128;
constexpr int take_note_velocity = 100;
constexpr int longest_note_gap_ms = 3;
// A positive note length keeps every guide period playable.
constexpr int shortest_note_ms = 1;
constexpr int longest_note_ms = 3;
constexpr int longest_completion_delay_ms = 3;
constexpr int longest_playback_pause_microseconds = 3000;
constexpr std::int64_t slot_command_steps = 30;

enum class SlotPhase {
    Muted,
    Armed,
    Looping,
};

struct SlotModel {
    SlotPhase phase{SlotPhase::Muted};
    // Keys of the armed or looping take.
    std::vector<int> keys;
};

struct SlotGroupModel {
    std::vector<SlotModel> slots;
    // Offset of the armed take's last Note Off.
    Milliseconds recorded_until{};
    // Keys are handed out in order, so no two takes share a key.
    int next_key{};
};

const char* phaseName(SlotPhase phase) {
    switch (phase) {
    case SlotPhase::Muted:
        return "Muted";
    case SlotPhase::Armed:
        return "Armed";
    case SlotPhase::Looping:
        return "Looping";
    }
    return "?";
}

std::ostream& operator<<(std::ostream& out, const SlotGroupModel& model) {
    out << '{';
    for (std::size_t id = 0; id < model.slots.size(); ++id) {
        const auto& slot = model.slots[id];
        out << (id == 0 ? "" : ", ") << "slot " << id << ": "
            << phaseName(slot.phase);
        for (const int key : slot.keys) {
            out << ' ' << key;
        }
    }
    return out << "; recorded until " << model.recorded_until.count() << "ms}";
}

struct StoppedTake {
    int channel{};
    std::vector<int> keys;
    // Call-log position of the all-notes-off with which the stop silenced
    // the slot channel.
    std::size_t silenced_at{};
};

Milliseconds drawMilliseconds(
    hegel::TestCase& tc,
    std::string_view name,
    int shortest_ms,
    int longest_ms
) {
    return Milliseconds(tc.draw(
        name,
        gs::integers<int>({.min_value = shortest_ms, .max_value = longest_ms}),
        repeatable
    ));
}

// Commands run on one thread, as the master FSM's mutex serializes them in
// production, while every slot worker plays its take on its own thread.
class LoopSlotStopMachine final
    : public hegel::stateful::StateMachine<
        LoopSlotStopMachine,
        SlotGroupModel
    > {
public:
    LoopSlotStopMachine()
        : StateMachine({.initial_state = SlotGroupModel{}}),
          config_{threeSlotConfig()},
          synth_engine_{config_},
          slots_{config_.loop_slots, synth_engine_} {
        state.slots.resize(config_.loop_slots.size());
    }

    std::vector<hegel::stateful::Rule<LoopSlotStopMachine>> rules() {
        using Rule = hegel::stateful::Rule<LoopSlotStopMachine>;

        return {
            Rule("arm a muted slot", [](hegel::TestCase& tc, auto& machine) {
                machine.armMutedSlot(tc);
            }),
            Rule("record a note", [](hegel::TestCase& tc, auto& machine) {
                machine.recordNote(tc);
            }),
            Rule("complete the take", [](hegel::TestCase& tc, auto& machine) {
                machine.completeTake(tc);
            }),
            Rule("cancel the take", [](hegel::TestCase& tc, auto& machine) {
                machine.cancelTake(tc);
            }),
            Rule("stop a looping slot", [](hegel::TestCase& tc, auto& machine) {
                machine.stopLoopingSlot(tc);
            }),
            Rule("let the loops play", [](hegel::TestCase& tc, auto&) {
                std::this_thread::sleep_for(drawDuration(
                    tc,
                    "pause",
                    0,
                    longest_playback_pause_microseconds
                ));
            }),
        };
    }

    std::vector<hegel::stateful::Invariant<LoopSlotStopMachine>>
    invariants() const {
        using Invariant = hegel::stateful::Invariant<LoopSlotStopMachine>;

        return {
            Invariant("no stopped take sounds again", [](const auto& machine) {
                const auto note = machine.noteAfterStop();
                if (note) {
                    throw std::runtime_error(*note);
                }
            }),
        };
    }

    // Terminating joins every worker, so no take can play afterwards.
    void terminate() {
        slots_.terminateAll();
    }

    std::optional<std::string> noteAfterStop() const {
        const auto calls = fake_fluidsynth::calls();
        for (const auto& take : stopped_takes_) {
            const auto after_silence = calls.begin()
                + static_cast<std::ptrdiff_t>(take.silenced_at) + 1;
            const auto note = std::find_if(
                after_silence,
                calls.end(),
                [&take](const Call& call) {
                    return call.kind == CallKind::SynthNoteOn
                        && call.channel == take.channel
                        && std::ranges::find(take.keys, call.key)
                            != take.keys.end();
                }
            );
            if (note != calls.end()) {
                return "channel " + std::to_string(take.channel)
                    + " played key " + std::to_string(note->key)
                    + " after its take was stopped";
            }
        }
        return std::nullopt;
    }

private:
    // The guide arms whenever it is muted; a regular slot arms only while
    // the guide loops.
    void armMutedSlot(hegel::TestCase& tc) {
        tc.assume(!armedSlot());
        const bool guide_looping =
            state.slots[guide_slot].phase == SlotPhase::Looping;
        std::vector<SlotId> armable;
        for (const auto id : slotsIn(SlotPhase::Muted)) {
            if (id == guide_slot || guide_looping) {
                armable.push_back(id);
            }
        }
        tc.assume(!armable.empty());
        const auto slot = tc.draw("slot", gs::sampled_from(armable), repeatable);

        requireOutcome(select(slot), LoopSlotSelectionOutcome::Armed);
        state.slots[slot] = SlotModel{.phase = SlotPhase::Armed, .keys = {}};
        state.recorded_until = Milliseconds::zero();
    }

    void recordNote(hegel::TestCase& tc) {
        const auto slot = armedSlot();
        tc.assume(slot.has_value() && state.next_key < midi_key_count);
        auto& take = state.slots[*slot];
        // The first note starts the take at offset zero.
        const auto gap = take.keys.empty()
            ? Milliseconds::zero()
            : drawMilliseconds(tc, "gap", 0, longest_note_gap_ms);
        const auto length = drawMilliseconds(
            tc,
            "length",
            shortest_note_ms,
            longest_note_ms
        );
        const int key = state.next_key++;
        const auto pressed_at = state.recorded_until + gap;

        slots_.recordNote(
            *slot,
            RecordedNoteKind::NoteOn,
            recordedNote(MidiMessageType::NoteOn, key, take_note_velocity),
            pressed_at
        );
        slots_.recordNote(
            *slot,
            RecordedNoteKind::NoteOff,
            recordedNote(MidiMessageType::NoteOff, key),
            pressed_at + length
        );
        take.keys.push_back(key);
        state.recorded_until = pressed_at + length;
    }

    // The take is recorded without waiting, so its timing places the
    // recording just before completion.
    void completeTake(hegel::TestCase& tc) {
        const auto slot = armedSlot();
        tc.assume(slot.has_value() && !state.slots[*slot].keys.empty());
        const auto completion_delay = drawMilliseconds(
            tc,
            "completion_delay",
            0,
            longest_completion_delay_ms
        );
        const auto completed_at = LooperClock::now();

        slots_.completeRecording(*slot, TakeTiming{
            .recording_started_at =
                completed_at - (state.recorded_until + completion_delay),
            .completed_at = completed_at,
        });
        state.slots[*slot].phase = SlotPhase::Looping;
    }

    // As in the master FSM, only a take with no notes yet is canceled.
    void cancelTake(hegel::TestCase& tc) {
        const auto slot = armedSlot();
        tc.assume(slot.has_value() && state.slots[*slot].keys.empty());

        slots_.cancelRecording(*slot);
        state.slots[*slot].phase = SlotPhase::Muted;
    }

    void stopLoopingSlot(hegel::TestCase& tc) {
        tc.assume(!armedSlot());
        const auto looping = slotsIn(SlotPhase::Looping);
        tc.assume(!looping.empty());
        const auto slot = tc.draw("slot", gs::sampled_from(looping), repeatable);

        requireOutcome(select(slot), LoopSlotSelectionOutcome::Stopped);
        // Stopping the guide stops every regular slot too.
        const auto stopped = slot == guide_slot
            ? looping
            : std::vector<SlotId>{slot};
        const auto calls = fake_fluidsynth::calls();
        for (const auto id : stopped) {
            const int channel = slots_.channel(id);
            stopped_takes_.push_back({
                .channel = channel,
                .keys = std::move(state.slots[id].keys),
                .silenced_at = lastSilence(calls, channel),
            });
            state.slots[id] = SlotModel{};
        }
    }

    // A stop silences the slot channel under the mutex the worker plays
    // under, so a Note On of the stopped take after that silence was played
    // after the stop. Measuring from the silence, not from the log size
    // when the stop returns, also catches a note played before the test
    // reads the log.
    static std::size_t lastSilence(const std::vector<Call>& calls, int channel) {
        const auto silence = std::find_if(
            calls.rbegin(),
            calls.rend(),
            [channel](const Call& call) {
                return call.kind == CallKind::SynthControlChange
                    && call.channel == channel
                    && call.control == all_notes_off_controller;
            }
        );
        if (silence == calls.rend()) {
            throw std::runtime_error(
                "a stopped slot did not silence its channel"
            );
        }
        return static_cast<std::size_t>(silence.base() - calls.begin()) - 1;
    }

    LoopSlotSelectionOutcome select(SlotId slot) {
        return slots_.requestSelection(
            config_.loop_slots[slot].key,
            config_.soundfonts.front(),
            transposer_
        ).outcome;
    }

    static void requireOutcome(
        LoopSlotSelectionOutcome actual,
        LoopSlotSelectionOutcome expected
    ) {
        if (actual != expected) {
            throw std::runtime_error(
                "the slot group's selection outcome disagrees with the model"
            );
        }
    }

    std::vector<SlotId> slotsIn(SlotPhase phase) const {
        std::vector<SlotId> ids;
        for (SlotId id = 0; id < state.slots.size(); ++id) {
            if (state.slots[id].phase == phase) {
                ids.push_back(id);
            }
        }
        return ids;
    }

    std::optional<SlotId> armedSlot() const {
        const auto armed = slotsIn(SlotPhase::Armed);
        if (armed.empty()) {
            return std::nullopt;
        }
        return armed.front();
    }

    // Slot selection and SynthEngine print to std::cout; discarding it keeps
    // a failing run's report readable.
    DiscardedStandardOutput discarded_output_;
    ApplicationConfig config_;
    SynthEngine synth_engine_;
    LoopSlotGroup slots_;
    OctaveTransposer transposer_;
    std::vector<StoppedTake> stopped_takes_;
};

TEST(CurrentBehaviorPropertyTest, StoppedTakeNeverSoundsAgain) {
    hegel::Settings settings;
    settings.stateful_step_count = slot_command_steps;

    hegel::test([](hegel::TestCase& tc) {
        fake_fluidsynth::reset();
        LoopSlotStopMachine machine;

        hegel::stateful::run(machine, tc);

        machine.terminate();
        ASSERT_EQ(machine.noteAfterStop(), std::nullopt);
    }, settings);
}

} // namespace
