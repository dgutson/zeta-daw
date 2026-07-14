#include "application.hpp"
#include "configuration.hpp"

#include <exception>
#include <iostream>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr const char* default_config_path = "/etc/zeta-daw/zeta.yaml";

sigset_t blockShutdownSignals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        throw std::runtime_error("Could not block shutdown signals");
    }
    return signals;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [config.yaml]\n";
        return 1;
    }

    try {
        const std::string config_path = argc == 2 ? argv[1] : default_config_path;
        auto config = zeta::loadConfiguration(config_path);
        const auto shutdown_signals = blockShutdownSignals();

        zeta::Application app{std::move(config)};
        std::jthread signal_waiter([&app, shutdown_signals](
            const std::stop_token& stop
        ) {
            int signal_number = 0;
            if (sigwait(&shutdown_signals, &signal_number) == 0
                && !stop.stop_requested()) {
                app.shutdownRequested();
            }
        });

        app.run();

        signal_waiter.request_stop();
        // The blocked signal wakes sigwait so the stop request can be observed.
        // NOLINTNEXTLINE(bugprone-bad-signal-to-kill-thread)
        pthread_kill(signal_waiter.native_handle(), SIGTERM);
        signal_waiter.join();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }
}
