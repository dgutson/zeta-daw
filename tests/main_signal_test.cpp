#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// zd starts and stops in milliseconds; the rest is margin for a loaded CI
// runner and ThreadSanitizer.
constexpr auto startup_timeout = 10s;
constexpr auto exit_timeout = 5s;
constexpr auto exit_poll_interval = 10ms;
constexpr std::string_view ready_line = "MIDI looper ready.";

// The fake FluidSynth reads no files, and loading the configuration does not
// check that they exist. Every loop slot starts a worker thread.
constexpr std::string_view configuration = R"(schema_version: 8
loop_slots:
  - C2
  - D2
soundfonts:
  - { id: piano, file: piano.sf2, bank: 0, preset: 0 }
controls:
  loop_slot_by_note: { type: machine_control, command: rewind }
  next_soundfont: { type: machine_control, command: stop }
  octave_down: { type: machine_control, command: play }
  octave_up: { type: machine_control, command: record_strobe }
)";

// Named after this process, so test processes running in parallel each have
// their own file.
class ConfigurationFile final {
public:
    ConfigurationFile()
        : path_(std::filesystem::temp_directory_path()
            / ("zeta-daw-signal-test-" + std::to_string(getpid()) + ".yaml")) {
        std::ofstream output{path_};
        output << configuration;
    }

    ~ConfigurationFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    ConfigurationFile(const ConfigurationFile&) = delete;
    ConfigurationFile& operator=(const ConfigurationFile&) = delete;

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// zd_with_fakes as a child process whose standard output and error arrive
// through one pipe. Destruction kills and reaps a child still running.
class ZdProcess final {
public:
    explicit ZdProcess(const std::filesystem::path& configuration_path) {
        std::array<int, 2> pipe_ends{};
        if (pipe2(pipe_ends.data(), O_CLOEXEC) != 0) {
            throw std::system_error(errno, std::generic_category(), "pipe2");
        }
        output_ = pipe_ends[0];
        const int write_end = pipe_ends[1];

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, write_end, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, write_end, STDERR_FILENO);

        // The child starts as from a shell or systemd, whatever ctest
        // inherited: no signal blocked, SIGINT and SIGTERM handled by default.
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        sigset_t signals;
        sigemptyset(&signals);
        posix_spawnattr_setsigmask(&attributes, &signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        posix_spawnattr_setsigdefault(&attributes, &signals);
        posix_spawnattr_setflags(
            &attributes,
            static_cast<short>(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF)
        );

        std::string program = ZETA_ZD_WITH_FAKES;
        std::string argument = configuration_path.string();
        std::array<char*, 3> arguments{
            program.data(),
            argument.data(),
            nullptr,
        };
        const int error = posix_spawn(
            &pid_,
            program.c_str(),
            &actions,
            &attributes,
            arguments.data(),
            environ
        );
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(write_end);
        if (error != 0) {
            close(output_);
            throw std::system_error(
                error,
                std::generic_category(),
                "posix_spawn " + program
            );
        }
    }

    ~ZdProcess() {
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
        close(output_);
    }

    ZdProcess(const ZdProcess&) = delete;
    ZdProcess& operator=(const ZdProcess&) = delete;

    void sendSignal(int signal_number) const {
        if (kill(pid_, signal_number) != 0) {
            throw std::system_error(errno, std::generic_category(), "kill");
        }
    }

    // False if the child closes its output or the deadline passes first.
    bool readUntil(std::string_view text, Clock::time_point deadline) {
        while (output_text_.find(text) == std::string::npos) {
            if (!readMore(deadline)) {
                return false;
            }
        }
        return true;
    }

    // Reads until the child closes its output, then reaps it. Returns the
    // wait status, or nothing while the child still runs at the deadline.
    std::optional<int> waitForExit(Clock::time_point deadline) {
        while (readMore(deadline)) {
        }
        while (true) {
            int status = 0;
            const pid_t reaped = waitpid(pid_, &status, WNOHANG);
            if (reaped == pid_) {
                pid_ = 0;
                return status;
            }
            if (Clock::now() >= deadline) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(exit_poll_interval);
        }
    }

    const std::string& output() const noexcept {
        return output_text_;
    }

private:
    pid_t pid_{};
    int output_{-1};
    std::string output_text_;

    // False at the end of the child's output or at the deadline.
    bool readMore(Clock::time_point deadline) {
        while (true) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                deadline - Clock::now()
            );
            if (remaining <= 0ms) {
                return false;
            }
            pollfd descriptor{.fd = output_, .events = POLLIN, .revents = 0};
            const int ready = poll(
                &descriptor,
                1,
                static_cast<int>(remaining.count())
            );
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0) {
                return false;
            }

            std::array<char, 4096> buffer{};
            const auto count = read(output_, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return false;
            }
            output_text_.append(buffer.data(), static_cast<std::size_t>(count));
            return true;
        }
    }
};

std::string describeWaitStatus(int status) {
    if (WIFEXITED(status)) {
        return "exited with status " + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return std::string{"ended by signal "} + strsignal(WTERMSIG(status));
    }
    return "wait status " + std::to_string(status);
}

// main.cpp blocks SIGINT and SIGTERM before constructing Application, so
// every thread it starts inherits the blocked mask, and its sigwait thread
// turns either signal into Application::shutdownRequested(). run() prints
// the ready line after that thread has started.
void expectStatusZeroAfter(int signal_number) {
    const ConfigurationFile configuration_file;
    ZdProcess zd{configuration_file.path()};

    ASSERT_TRUE(zd.readUntil(ready_line, Clock::now() + startup_timeout))
        << "No ready line; zd's output:\n" << zd.output();
    zd.sendSignal(signal_number);
    const auto status = zd.waitForExit(Clock::now() + exit_timeout);

    ASSERT_TRUE(status.has_value())
        << "zd still running " << exit_timeout.count()
        << " s after the signal; its output:\n" << zd.output();
    EXPECT_TRUE(WIFEXITED(*status) && WEXITSTATUS(*status) == 0)
        << "zd " << describeWaitStatus(*status) << "; its output:\n"
        << zd.output();
}

TEST(MainSignalTest, SigintShutsDownAndExitsWithStatusZero) {
    expectStatusZeroAfter(SIGINT);
}

TEST(MainSignalTest, SigtermShutsDownAndExitsWithStatusZero) {
    expectStatusZeroAfter(SIGTERM);
}

} // namespace
