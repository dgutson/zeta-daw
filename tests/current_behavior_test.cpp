#include "fake_fluidsynth.hpp"
#include "../application.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <iostream>
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
using zeta::MidiMessageType;

constexpr int raw(MidiMessageType type) {
    return static_cast<int>(type);
}

class BlockingInputBuffer final : public std::streambuf {
public:
    void push(char character) {
        {
            std::lock_guard lock(mutex_);
            characters_.push_back(character);
        }
        changed_.notify_all();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        changed_.notify_all();
    }

protected:
    int_type underflow() override {
        if (gptr() && gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }

        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] {
            return closed_ || !characters_.empty();
        });

        if (characters_.empty()) {
            return traits_type::eof();
        }

        current_ = characters_.front();
        characters_.pop_front();
        setg(&current_, &current_, &current_ + 1);
        return traits_type::to_int_type(current_);
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<char> characters_;
    char current_{};
    bool closed_{};
};

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
        : old_input_(std::cin.rdbuf(&input_)),
          old_output_(std::cout.rdbuf(&output_)),
          old_error_(std::cerr.rdbuf(&error_)),
          thread_([&application] {
              application.runInteractive();
          })
    {
        std::cin.clear();
    }

    ~InteractiveSession() {
        input_.close();
        if (thread_.joinable()) {
            thread_.join();
        }

        std::cin.rdbuf(old_input_);
        std::cout.rdbuf(old_output_);
        std::cerr.rdbuf(old_error_);
        std::cin.clear();
    }

    InteractiveSession(const InteractiveSession&) = delete;
    InteractiveSession& operator=(const InteractiveSession&) = delete;

    void pressEnter() {
        input_.push('\n');
    }

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
    BlockingInputBuffer input_;
    CapturingOutputBuffer output_;
    CapturingOutputBuffer error_;
    std::streambuf* old_input_;
    std::streambuf* old_output_;
    std::streambuf* old_error_;
    std::jthread thread_;
};

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
    }
};

TEST_F(CurrentBehaviorTest, LoadsDedicatedProgramsForLoopAndLiveChannels) {
    {
        Application application{"loop.sf2", "live.sf2"};

        const auto calls = fake_fluidsynth::calls();
        ASSERT_GE(calls.size(), 4U);

        EXPECT_EQ(calls[0].kind, CallKind::LoadSoundFont);
        EXPECT_EQ(calls[0].text, "loop.sf2");
        EXPECT_EQ(calls[1].kind, CallKind::SelectProgram);
        EXPECT_EQ(calls[1].channel, 1);
        EXPECT_EQ(calls[1].bank, 0);
        EXPECT_EQ(calls[1].preset, 34);

        EXPECT_EQ(calls[2].kind, CallKind::LoadSoundFont);
        EXPECT_EQ(calls[2].text, "live.sf2");
        EXPECT_EQ(calls[3].kind, CallKind::SelectProgram);
        EXPECT_EQ(calls[3].channel, 0);
        EXPECT_EQ(calls[3].bank, 0);
        EXPECT_EQ(calls[3].preset, 0);
    }

    const auto calls = fake_fluidsynth::calls();
    const auto midi_deleted = firstIndexOf(calls, CallKind::DeleteMidiDriver);
    const auto audio_deleted = firstIndexOf(calls, CallKind::DeleteAudioDriver);
    const auto synth_deleted = firstIndexOf(calls, CallKind::DeleteSynth);
    const auto settings_deleted = firstIndexOf(calls, CallKind::DeleteSettings);

    EXPECT_LT(midi_deleted, audio_deleted);
    EXPECT_LT(audio_deleted, synth_deleted);
    EXPECT_LT(synth_deleted, settings_deleted);
}

TEST_F(CurrentBehaviorTest, ReadyRoutesMidiToDedicatedLiveChannel) {
    Application application{"loop.sf2", "live.sf2"};

    EXPECT_EQ(fake_fluidsynth::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 7,
        .key = 64,
        .velocity = 91,
    }), FLUID_OK);

    const auto calls = fake_fluidsynth::calls();
    EXPECT_TRUE(hasCall(calls, CallKind::HandleMidi, 0, 64));
    EXPECT_FALSE(hasCall(calls, CallKind::HandleMidi, 7, 64));
    EXPECT_FALSE(hasCall(calls, CallKind::SynthNoteOn));
}

TEST_F(CurrentBehaviorTest, SuppressesZeroExpressionBeforeItReachesTheSynth) {
    Application application{"loop.sf2", "live.sf2"};

    const auto before = fake_fluidsynth::calls();
    const auto handled_before = std::ranges::count(before, CallKind::HandleMidi, &Call::kind);

    EXPECT_EQ(fake_fluidsynth::emitMidi({
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

    EXPECT_EQ(fake_fluidsynth::emitMidi({
        .type = raw(MidiMessageType::ControlChange),
        .channel = 0,
        .control = 11,
        .value = 1,
    }), FLUID_OK);

    const auto after_forwarded = fake_fluidsynth::calls();
    EXPECT_TRUE(hasCall(after_forwarded, CallKind::HandleMidi, 0, std::nullopt, 1));
}

TEST_F(CurrentBehaviorTest, ArmedMonitorsOnLoopChannelButDoesNotStartOnNoteOff) {
    Application application{"loop.sf2", "live.sf2"};
    InteractiveSession session{application};

    session.pressEnter();
    ASSERT_TRUE(session.waitForOutput("Recording..."));

    EXPECT_EQ(fake_fluidsynth::emitMidi({
        .type = raw(MidiMessageType::NoteOff),
        .channel = 0,
        .key = 60,
    }), FLUID_OK);
    EXPECT_EQ(fake_fluidsynth::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 0,
        .key = 60,
        .velocity = 0,
    }), FLUID_OK);
    EXPECT_EQ(fake_fluidsynth::emitMidi({
        .type = raw(MidiMessageType::ControlChange),
        .channel = 0,
        .control = 7,
        .value = 99,
    }), FLUID_OK);

    session.pressEnter();
    session.join();

    const auto calls = fake_fluidsynth::calls();
    EXPECT_TRUE(hasCall(calls, CallKind::HandleMidi, 1, 60));
    EXPECT_TRUE(hasCall(calls, CallKind::HandleMidi, 1, std::nullopt, 99));
    EXPECT_FALSE(hasCall(calls, CallKind::SynthNoteOn));
    EXPECT_THAT(session.output(), ::testing::HasSubstr("No notes were recorded. Exiting."));
}

TEST_F(CurrentBehaviorTest, FirstNoteStartsRecordingAndCompletedTakeLoopsOnChannelOne) {
    Application application{"loop.sf2", "live.sf2"};
    InteractiveSession session{application};

    session.pressEnter();
    ASSERT_TRUE(session.waitForOutput("Recording..."));

    EXPECT_EQ(fake_fluidsynth::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 0,
        .key = 60,
        .velocity = 100,
    }), FLUID_OK);

    std::this_thread::sleep_for(15ms);

    EXPECT_EQ(fake_fluidsynth::emitMidi({
        .type = raw(MidiMessageType::NoteOff),
        .channel = 0,
        .key = 60,
    }), FLUID_OK);

    std::this_thread::sleep_for(10ms);
    session.pressEnter();
    ASSERT_TRUE(session.waitForOutput("Looping."));

    ASSERT_TRUE(fake_fluidsynth::waitUntil([](const std::vector<Call>& calls) {
        return hasCall(calls, CallKind::SynthNoteOn, 1, 60)
            && hasCall(calls, CallKind::SynthNoteOff, 1, 60);
    }));

    EXPECT_EQ(fake_fluidsynth::emitMidi({
        .type = raw(MidiMessageType::NoteOn),
        .channel = 1,
        .key = 72,
        .velocity = 80,
    }), FLUID_OK);
    EXPECT_EQ(fake_fluidsynth::emitMidi({
        .type = raw(MidiMessageType::PitchBend),
        .channel = 1,
        .pitch = 16383,
    }), FLUID_OK);

    session.pressEnter();
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
    EXPECT_THAT(session.output(), ::testing::HasSubstr("Press ENTER to stop and quit."));
}

} // namespace
