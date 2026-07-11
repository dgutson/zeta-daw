#include "midi_input.hpp"

#include <libremidi/libremidi.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zeta {
namespace {

constexpr auto midi_api = libremidi::API::ALSA_SEQ;
constexpr std::string_view client_name = "zeta-daw";

bool samePort(
    const libremidi::input_port& left,
    const libremidi::input_port& right
) noexcept {
    return left.api == right.api && left.port == right.port;
}

class LibremidiInput final : public MidiInput {
public:
    ~LibremidiInput() override {
        stop();
    }

    void start(Handler handler) override {
        std::lock_guard lock(mutex_);
        if (observer_) {
            throw std::logic_error("MIDI input is already running");
        }

        handler_ = std::move(handler);
        stopping_ = false;

        libremidi::observer_configuration configuration{
            .on_error = [](std::string_view error, const auto&) {
                std::cerr << "[MIDI input error] " << error << '\n';
            },
            .on_warning = [](std::string_view warning, const auto&) {
                std::cerr << "[MIDI input warning] " << warning << '\n';
            },
            .input_added = [this](const libremidi::input_port& port) {
                connect(port);
            },
            .input_removed = [this](const libremidi::input_port& port) {
                disconnect(port);
            },
            .track_hardware = true,
            .track_virtual = false,
            .track_network = false,
            .track_any = false,
            .notify_in_constructor = false,
        };

        observer_ = std::make_unique<libremidi::observer>(
            configuration,
            libremidi::observer_configuration_for(midi_api)
        );

        const auto ports = observer_->get_input_ports();
        for (const auto& port : ports) {
            connectUnlocked(port);
        }
    }

    void stop() noexcept override {
        std::vector<Connection> inputs;
        {
            std::lock_guard lock(mutex_);
            if (!observer_) {
                return;
            }
            stopping_ = true;
            handler_ = {};
            inputs.swap(inputs_);
        }

        inputs.clear();
        observer_.reset();
    }

private:
    struct Connection {
        libremidi::input_port port;
        std::unique_ptr<libremidi::midi_in> input;
    };

    void connect(const libremidi::input_port& port) {
        std::lock_guard lock(mutex_);
        connectUnlocked(port);
    }

    void connectUnlocked(const libremidi::input_port& port) {
        if (stopping_ || !observer_) {
            return;
        }

        const auto already_connected = std::ranges::any_of(
            inputs_,
            [&](const Connection& connection) {
                return samePort(connection.port, port);
            }
        );
        if (already_connected) {
            return;
        }

        libremidi::input_configuration configuration{
            .on_message = [this](libremidi::message&& message) {
                receive(message);
            },
            .on_error = [](std::string_view error, const auto&) {
                std::cerr << "[MIDI input error] " << error << '\n';
            },
            .on_warning = [](std::string_view warning, const auto&) {
                std::cerr << "[MIDI input warning] " << warning << '\n';
            },
            .ignore_sysex = false,
            .ignore_timing = true,
            .ignore_sensing = true,
            .timestamps = libremidi::SystemMonotonic,
        };

        auto input = std::make_unique<libremidi::midi_in>(
            configuration,
            libremidi::midi_in_configuration_for(midi_api)
        );

        const auto error = input->open_port(port, client_name);
        if (error.is_set()) {
            std::cerr << "[MIDI input] could not open " << port.display_name << '\n';
            return;
        }

        std::cout << "[MIDI input] connected: " << port.display_name << '\n';
        inputs_.push_back({port, std::move(input)});
    }

    void disconnect(const libremidi::input_port& port) {
        std::unique_ptr<libremidi::midi_in> input;
        std::string display_name;
        {
            std::lock_guard lock(mutex_);
            const auto connection = std::ranges::find_if(
                inputs_,
                [&](const Connection& candidate) {
                    return samePort(candidate.port, port);
                }
            );
            if (connection == inputs_.end()) {
                return;
            }

            display_name = connection->port.display_name;
            input = std::move(connection->input);
            inputs_.erase(connection);
        }

        input.reset();
        std::cout << "[MIDI input] disconnected: " << display_name << '\n';
    }

    void receive(const libremidi::message& message) noexcept {
        try {
            const auto event = decodeMidiEvent(message.bytes);
            if (!event) {
                return;
            }

            Handler handler;
            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    return;
                }
                handler = handler_;
            }
            if (handler) {
                handler(*event);
            }
        } catch (const std::exception& error) {
            std::cerr << "[MIDI input error] " << error.what() << '\n';
        } catch (...) {
            std::cerr << "[MIDI input error] unknown callback failure\n";
        }
    }

    std::mutex mutex_;
    Handler handler_;
    std::unique_ptr<libremidi::observer> observer_;
    std::vector<Connection> inputs_;
    bool stopping_{true};
};

} // namespace

std::unique_ptr<MidiInput> makeMidiInput() {
    return std::make_unique<LibremidiInput>();
}

} // namespace zeta
