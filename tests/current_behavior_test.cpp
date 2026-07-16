#include "fake_fluidsynth.hpp"
#include "fake_midi_input.hpp"
#include "../application.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
#include <vector>

namespace {

using namespace std::chrono_literals;
using fake_fluidsynth::Call;
using fake_fluidsynth::CallKind;
using zeta::Application;
using zeta::ApplicationConfig;
using zeta::LoopSlotDefinition;
using zeta::MidiControlBinding;
using zeta::MidiControlType;
using zeta::MidiEvent;
using zeta::MidiInput;
using zeta::MidiMessageType;
using zeta::SoundFontDefinition;

constexpr int first_slot_key = 48;
constexpr int second_slot_key = 50;
constexpr int first_slot_channel = 1;
constexpr int second_slot_channel = 2;

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

ApplicationConfig testConfig() {
    return {
        .loop_slots = {
            LoopSlotDefinition{.key = first_slot_key},
            LoopSlotDefinition{.key = second_slot_key},
        },
        .soundfonts = {
            SoundFontDefinition{
                .id = "piano",
                .file = "piano.sf2",
                .bank = 0,
                .preset = 0,
                .key = 71,
            },
            SoundFontDefinition{
                .id = "bass",
                .file = "bass.sf2",
                .bank = 0,
                .preset = 34,
                .key = 72,
            },
        },
        .midi_control_change_mappings = {},
        .loop_slot_by_note_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x05,
        },
        .next_soundfont_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x01,
        },
        .soundfont_by_note_control = MidiControlBinding{
            .type = MidiControlType::MachineControl,
            .number = 0x09,
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

std::size_t callCount(CallKind kind, int channel, int key) {
    return static_cast<std::size_t>(std::ranges::count_if(
        fake_fluidsynth::calls(),
        [&](const Call& call) {
            return call.kind == kind
                && call.channel == channel
                && call.key == key;
        }
    ));
}

std::size_t controlChangeCount(int channel, int control) {
    return static_cast<std::size_t>(std::ranges::count_if(
        fake_fluidsynth::calls(),
        [&](const Call& call) {
            return call.kind == CallKind::SynthControlChange
                && call.channel == channel
                && call.control == control;
        }
    ));
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

bool waitForNoteCount(
    int channel,
    int key,
    std::size_t count,
    std::chrono::milliseconds timeout = 1s
) {
    return fake_fluidsynth::waitUntil(
        [&](const std::vector<Call>& calls) {
            return static_cast<std::size_t>(std::ranges::count_if(
                calls,
                [&](const Call& call) {
                    return call.kind == CallKind::SynthNoteOn
                        && call.channel == channel
                        && call.key == key;
                }
            )) >= count;
        },
        timeout
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
        EXPECT_EQ(calls.front().kind, CallKind::ConfigureMidiChannels);
        EXPECT_EQ(calls.front().value, 16);
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

TEST_F(CurrentBehaviorTest, StoppingOneSlotDoesNotAffectItsPeer) {
    Application application{testConfig(), fake_midi_input::makeInput()};
    completeTake(first_slot_key, 60, 10ms);
    completeTake(second_slot_key, 64, 10ms);
    ASSERT_TRUE(waitForNoteCount(first_slot_channel, 60, 2));
    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, 2));

    selectSlot(first_slot_key);
    const auto stopped_count = callCount(
        CallKind::SynthNoteOn,
        first_slot_channel,
        60
    );
    const auto peer_count = callCount(
        CallKind::SynthNoteOn,
        second_slot_channel,
        64
    );

    ASSERT_TRUE(waitForNoteCount(second_slot_channel, 64, peer_count + 2));
    EXPECT_EQ(
        callCount(CallKind::SynthNoteOn, first_slot_channel, 60),
        stopped_count
    );
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

} // namespace
