#pragma once

#include <systemd/sd-bus.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

// A private D-Bus session bus with a fake org.freedesktop.ScreenSaver on it,
// for the tests of ScreenLockInhibitor and of zd holding its inhibit.
namespace fake_screen_saver {

// A dbus-daemon that this process starts and that activates no services, so
// no test reaches the desktop's bus or starts a desktop service. It ends
// when this object is destroyed or this process ends.
class PrivateSessionBus final {
public:
    PrivateSessionBus();
    ~PrivateSessionBus();

    PrivateSessionBus(const PrivateSessionBus&) = delete;
    PrivateSessionBus& operator=(const PrivateSessionBus&) = delete;

    // The address, as DBUS_SESSION_BUS_ADDRESS gives it.
    const std::string& address() const noexcept {
        return address_;
    }

private:
    pid_t pid_{};
    std::string address_;
};

// An Inhibit or UnInhibit call that the fake screen saver answered.
struct Call {
    std::string method;
    // Inhibit's arguments; empty for UnInhibit.
    std::string application;
    std::string reason;
    // The cookie that Inhibit answered or UnInhibit received.
    std::uint32_t cookie{};
};

// org.freedesktop.ScreenSaver on the bus at an address. From a thread of its
// own, it answers Inhibit with a new cookie and UnInhibit with nothing, and
// records each call. Made with Answers::never, it owns the name but reads no
// call, like a desktop that hangs.
class FakeScreenSaver final {
public:
    enum class Answers { always, never };

    explicit FakeScreenSaver(
        const std::string& bus_address,
        Answers answers = Answers::always
    );

    FakeScreenSaver(const FakeScreenSaver&) = delete;
    FakeScreenSaver& operator=(const FakeScreenSaver&) = delete;

    std::vector<Call> calls() const;

private:
    struct BusDeleter {
        void operator()(sd_bus* bus) const noexcept {
            sd_bus_flush_close_unref(bus);
        }
    };

    struct SlotDeleter {
        void operator()(sd_bus_slot* slot) const noexcept {
            sd_bus_slot_unref(slot);
        }
    };

    std::unique_ptr<sd_bus, BusDeleter> bus_;
    std::unique_ptr<sd_bus_slot, SlotDeleter> object_;
    mutable std::mutex mutex_;
    std::vector<Call> calls_;
    // Used only by the server thread.
    std::uint32_t next_cookie_;
    // Declared last, so the thread stops before the members it uses go.
    std::jthread server_;

    static int handle(sd_bus_message* message, void* fake, sd_bus_error* error);
    void record(Call call);
    void serve(const std::stop_token& stop);
};

} // namespace fake_screen_saver
