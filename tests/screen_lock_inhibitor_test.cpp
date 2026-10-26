#include "../screen_lock_inhibitor.hpp"
#include "fake_screen_saver.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using fake_screen_saver::FakeScreenSaver;
using fake_screen_saver::PrivateSessionBus;

constexpr std::string_view held_line =
    "[screen lock] held: the desktop will not lock the screen\n";
constexpr std::string_view not_held_prefix = "[screen lock] not held: ";
// screen_lock_inhibitor.cpp's call timeout, plus margin for a loaded CI
// runner.
constexpr auto startup_limit = 1s + 1s;

// Sets an environment variable, or unsets it when value is null, and
// restores it on destruction. Use it only while no other thread runs, since
// setenv races getenv.
class ScopedVariable final {
public:
    ScopedVariable(const char* name, const char* value) : name_(name) {
        const char* old_value = std::getenv(name);
        if (old_value != nullptr) {
            old_value_ = old_value;
        }
        if (value != nullptr) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
    }

    ~ScopedVariable() {
        if (old_value_) {
            setenv(name_, old_value_->c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }

    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_value_;
};

// A private session bus that sd_bus_open_user finds through
// DBUS_SESSION_BUS_ADDRESS, which it reads before XDG_RUNTIME_DIR.
struct SessionBus {
    PrivateSessionBus bus;
    ScopedVariable address{"DBUS_SESSION_BUS_ADDRESS", bus.address().c_str()};
};

// One line that starts with the prefix and has a reason after it.
void expectOneNotHeldLine(const std::string& printed) {
    EXPECT_TRUE(printed.starts_with(not_held_prefix)) << printed;
    EXPECT_GT(printed.size(), not_held_prefix.size() + 1) << printed;
    EXPECT_EQ(printed.find('\n'), printed.size() - 1) << printed;
}

TEST(ScreenLockInhibitorTest, HoldsTheInhibitAsZdWithAReason) {
    const SessionBus session;
    const FakeScreenSaver screen_saver{session.bus.address()};
    std::ostringstream log;

    const zeta::ScreenLockInhibitor inhibitor{log};

    const auto calls = screen_saver.calls();
    ASSERT_EQ(calls.size(), 1U);
    EXPECT_EQ(calls[0].method, "Inhibit");
    EXPECT_EQ(calls[0].application, "zd");
    // GNOME does not answer an inhibit whose reason is empty.
    EXPECT_FALSE(calls[0].reason.empty());
    EXPECT_EQ(log.str(), held_line);
}

TEST(ScreenLockInhibitorTest, ReleasesTheSameCookieWhenDestroyed) {
    const SessionBus session;
    const FakeScreenSaver screen_saver{session.bus.address()};
    std::ostringstream log;

    {
        const zeta::ScreenLockInhibitor inhibitor{log};
    }

    const auto calls = screen_saver.calls();
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[1].method, "UnInhibit");
    EXPECT_EQ(calls[1].cookie, calls[0].cookie);
}

// As for the Raspberry Pi's system service.
TEST(ScreenLockInhibitorTest, WithoutASessionBusPrintsOneLine) {
    const ScopedVariable no_address{"DBUS_SESSION_BUS_ADDRESS", nullptr};
    const ScopedVariable no_runtime_directory{"XDG_RUNTIME_DIR", nullptr};
    std::ostringstream log;

    const zeta::ScreenLockInhibitor inhibitor{log};

    EXPECT_EQ(log.str(), "[screen lock] not held: no desktop session bus\n");
}

// As on a desktop that provides no org.freedesktop.ScreenSaver.
TEST(ScreenLockInhibitorTest, WithoutTheServicePrintsTheBusError) {
    const SessionBus session;
    std::ostringstream log;

    const zeta::ScreenLockInhibitor inhibitor{log};

    expectOneNotHeldLine(log.str());
}

TEST(ScreenLockInhibitorTest, AServiceThatNeverAnswersDelaysStartupByAtMostTheTimeout) {
    const SessionBus session;
    const FakeScreenSaver screen_saver{
        session.bus.address(),
        FakeScreenSaver::Answers::never,
    };
    std::ostringstream log;

    const auto started = std::chrono::steady_clock::now();
    const zeta::ScreenLockInhibitor inhibitor{log};
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, startup_limit);
    expectOneNotHeldLine(log.str());
}

} // namespace
