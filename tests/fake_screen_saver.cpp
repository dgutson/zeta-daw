#include "fake_screen_saver.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string_view>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace fake_screen_saver {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// No service activation and no included files: only the services that a
// test starts exist on this bus.
constexpr std::string_view bus_configuration = R"(<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
)";

// dbus-daemon starts in milliseconds; the rest is margin for a loaded CI
// runner and ThreadSanitizer.
constexpr auto bus_startup_timeout = 10s;

constexpr const char* screen_saver_service = "org.freedesktop.ScreenSaver";
constexpr const char* screen_saver_object = "/org/freedesktop/ScreenSaver";
constexpr const char* screen_saver_interface = "org.freedesktop.ScreenSaver";

// Not 0, so that releasing a cookie that was never set shows.
constexpr std::uint32_t first_cookie = 7001;
// How often the server thread checks for a stop request while no call
// arrives.
constexpr std::uint64_t stop_check_microseconds = 10'000;

void check(int result, const char* what) {
    if (result < 0) {
        throw std::system_error(-result, std::generic_category(), what);
    }
}

// The first line that input delivers before the deadline, without its line
// feed; empty when input closes or the deadline passes first.
std::string readLine(int input, Clock::time_point deadline) {
    std::string line;
    while (true) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - Clock::now()
        );
        if (remaining <= 0ms) {
            return {};
        }
        pollfd descriptor{.fd = input, .events = POLLIN, .revents = 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return {};
        }
        char character = 0;
        const auto count = read(input, &character, 1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return {};
        }
        if (character == '\n') {
            return line;
        }
        line.push_back(character);
    }
}

} // namespace

PrivateSessionBus::PrivateSessionBus() {
    // Named after this process, so test processes running in parallel each
    // have their own file. dbus-daemon reads it only while starting.
    const auto configuration = std::filesystem::temp_directory_path()
        / ("zeta-daw-session-bus-" + std::to_string(getpid()) + ".conf");
    {
        std::ofstream output{configuration};
        output << bus_configuration;
    }

    std::array<int, 2> pipe_ends{};
    if (pipe2(pipe_ends.data(), O_CLOEXEC) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe2");
    }
    std::string program = "dbus-daemon";
    std::string no_fork = "--nofork";
    std::string print_address = "--print-address";
    std::string configuration_file = "--config-file=" + configuration.string();
    std::array<char*, 5> arguments{
        program.data(),
        no_fork.data(),
        print_address.data(),
        configuration_file.data(),
        nullptr,
    };

    const pid_t parent = getpid();
    pid_ = fork();
    if (pid_ == 0) {
        // Killed when this process ends, a crash included, so no daemon is
        // left behind.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) {
            _exit(1);
        }
        dup2(pipe_ends[1], STDOUT_FILENO);
        execvp(program.c_str(), arguments.data());
        _exit(127);
    }
    const int fork_error = errno;
    close(pipe_ends[1]);
    if (pid_ > 0) {
        address_ = readLine(pipe_ends[0], Clock::now() + bus_startup_timeout);
    }
    close(pipe_ends[0]);
    std::error_code ignored;
    std::filesystem::remove(configuration, ignored);

    if (pid_ < 0) {
        throw std::system_error(fork_error, std::generic_category(), "fork");
    }
    if (address_.empty()) {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
        throw std::runtime_error(
            "dbus-daemon printed no bus address; these tests need the "
            "dbus-daemon package"
        );
    }
}

PrivateSessionBus::~PrivateSessionBus() {
    kill(pid_, SIGTERM);
    waitpid(pid_, nullptr, 0);
}

FakeScreenSaver::FakeScreenSaver(const std::string& bus_address, Answers answers)
    : next_cookie_(first_cookie) {
    sd_bus* bus = nullptr;
    check(sd_bus_new(&bus), "sd_bus_new");
    bus_.reset(bus);
    check(sd_bus_set_address(bus, bus_address.c_str()), "sd_bus_set_address");
    check(sd_bus_set_bus_client(bus, 1), "sd_bus_set_bus_client");
    check(sd_bus_start(bus), "sd_bus_start");

    sd_bus_slot* object = nullptr;
    check(
        sd_bus_add_object(bus, &object, screen_saver_object, &FakeScreenSaver::handle, this),
        "sd_bus_add_object"
    );
    object_.reset(object);
    check(sd_bus_request_name(bus, screen_saver_service, 0), "sd_bus_request_name");

    if (answers == Answers::always) {
        server_ = std::jthread([this](const std::stop_token& stop) {
            serve(stop);
        });
    }
}

std::vector<Call> FakeScreenSaver::calls() const {
    const std::lock_guard lock{mutex_};
    return calls_;
}

// Records a call before answering it, so a caller whose call has returned
// finds it recorded.
int FakeScreenSaver::handle(sd_bus_message* message, void* fake, sd_bus_error*) {
    auto& self = *static_cast<FakeScreenSaver*>(fake);
    if (sd_bus_message_is_method_call(message, screen_saver_interface, "Inhibit") > 0) {
        const char* application = nullptr;
        const char* reason = nullptr;
        const int read_result = sd_bus_message_read(message, "ss", &application, &reason);
        if (read_result < 0) {
            return read_result;
        }
        const std::uint32_t cookie = self.next_cookie_++;
        self.record({"Inhibit", application, reason, cookie});
        return sd_bus_reply_method_return(message, "u", cookie);
    }
    if (sd_bus_message_is_method_call(message, screen_saver_interface, "UnInhibit") > 0) {
        std::uint32_t cookie = 0;
        const int read_result = sd_bus_message_read(message, "u", &cookie);
        if (read_result < 0) {
            return read_result;
        }
        self.record({"UnInhibit", {}, {}, cookie});
        return sd_bus_reply_method_return(message, "");
    }
    // sd-bus answers any other method with an unknown-method error.
    return 0;
}

void FakeScreenSaver::record(Call call) {
    const std::lock_guard lock{mutex_};
    calls_.push_back(std::move(call));
}

void FakeScreenSaver::serve(const std::stop_token& stop) {
    while (!stop.stop_requested()) {
        const int processed = sd_bus_process(bus_.get(), nullptr);
        if (processed < 0) {
            return;
        }
        if (processed == 0) {
            sd_bus_wait(bus_.get(), stop_check_microseconds);
        }
    }
}

} // namespace fake_screen_saver
