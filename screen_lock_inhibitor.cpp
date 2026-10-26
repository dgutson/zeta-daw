#include "screen_lock_inhibitor.hpp"

#include <systemd/sd-bus.h>

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>

namespace zeta {
namespace {

// The service, object and interface of the freedesktop Idle Inhibition
// specification, which XFCE, GNOME and KDE provide.
constexpr const char* screen_saver_service = "org.freedesktop.ScreenSaver";
constexpr const char* screen_saver_object = "/org/freedesktop/ScreenSaver";
constexpr const char* screen_saver_interface = "org.freedesktop.ScreenSaver";

constexpr const char* application_name = "zd";
// GNOME does not answer an inhibit whose reason is empty.
constexpr const char* inhibit_reason = "Live performance";

// 1 s, the longest a desktop that does not answer delays startup or
// shutdown, in the microseconds sd-bus counts; its default is 25 s.
constexpr std::uint64_t call_timeout_microseconds = 1'000'000;

struct BusDeleter {
    void operator()(sd_bus* bus) const noexcept {
        sd_bus_flush_close_unref(bus);
    }
};

struct MessageDeleter {
    void operator()(sd_bus_message* message) const noexcept {
        sd_bus_message_unref(message);
    }
};

using Bus = std::unique_ptr<sd_bus, BusDeleter>;
using Message = std::unique_ptr<sd_bus_message, MessageDeleter>;

// A call's error, freed on destruction. It starts zeroed, as
// SD_BUS_ERROR_NULL would set it, which is a C compound literal.
class CallError final {
public:
    CallError() = default;
    ~CallError() {
        sd_bus_error_free(&error_);
    }

    CallError(const CallError&) = delete;
    CallError& operator=(const CallError&) = delete;

    sd_bus_error* get() noexcept {
        return &error_;
    }

    // The desktop's message, or the text of the call's negative result when
    // the call failed without one.
    std::string describe(int result) const {
        if (error_.message != nullptr) {
            return error_.message;
        }
        return std::generic_category().message(-result);
    }

private:
    sd_bus_error error_{};
};

} // namespace

struct ScreenLockInhibitor::Impl {
    // Empty while no inhibit is held.
    Bus bus;
    std::uint32_t cookie{};
};

ScreenLockInhibitor::ScreenLockInhibitor(std::ostream& log)
    : impl_(std::make_unique<Impl>()) {
    sd_bus* opened = nullptr;
    const int open_result = sd_bus_open_user(&opened);
    if (open_result < 0) {
        log << "[screen lock] not held: no desktop session bus\n";
        return;
    }
    Bus bus{opened};
    sd_bus_set_method_call_timeout(bus.get(), call_timeout_microseconds);

    CallError error;
    sd_bus_message* answered = nullptr;
    int result = sd_bus_call_method(
        bus.get(),
        screen_saver_service,
        screen_saver_object,
        screen_saver_interface,
        "Inhibit",
        error.get(),
        &answered,
        "ss",
        application_name,
        inhibit_reason
    );
    const Message answer{answered};
    if (result >= 0) {
        result = sd_bus_message_read(answer.get(), "u", &impl_->cookie);
    }
    if (result < 0) {
        log << "[screen lock] not held: " << error.describe(result) << '\n';
        return;
    }
    impl_->bus = std::move(bus);
    log << "[screen lock] held: the desktop will not lock the screen\n";
}

ScreenLockInhibitor::~ScreenLockInhibitor() {
    if (!impl_->bus) {
        return;
    }
    // Closing the connection releases the inhibit as well, so a failed
    // UnInhibit needs nothing more.
    sd_bus_call_method(
        impl_->bus.get(),
        screen_saver_service,
        screen_saver_object,
        screen_saver_interface,
        "UnInhibit",
        nullptr,
        nullptr,
        "u",
        impl_->cookie
    );
}

} // namespace zeta
