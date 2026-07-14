#include "fake_midi_input.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace {

class FakeMidiInput;

std::mutex mutex;
FakeMidiInput* active_input{};

class FakeMidiInput final : public zeta::MidiInput {
public:
    ~FakeMidiInput() override {
        stop();
    }

    void start(Handler handler) override {
        std::lock_guard lock(mutex);
        handler_ = std::move(handler);
        active_input = this;
    }

    void stop() noexcept override {
        std::lock_guard lock(mutex);
        if (active_input == this) {
            active_input = nullptr;
        }
        handler_ = {};
    }

    Handler handler() const {
        return handler_;
    }

private:
    Handler handler_;
};

} // namespace

namespace fake_midi_input {

std::unique_ptr<zeta::MidiInput> makeInput() {
    return std::make_unique<FakeMidiInput>();
}

void reset() {
    std::lock_guard lock(mutex);
    active_input = nullptr;
}

int emitMidi(const MidiEvent& source) {
    zeta::MidiInput::Handler handler;
    {
        std::lock_guard lock(mutex);
        if (!active_input) {
            throw std::logic_error("No active fake MIDI input");
        }
        handler = active_input->handler();
    }

    const auto type = source.type == static_cast<int>(zeta::MidiMessageType::MachineControl)
        ? zeta::MidiMessageType::MachineControl
        : zeta::classifyMidiMessage(source.type);
    handler({
        .type = type,
        .message = {
            .raw_type = source.type,
            .channel = source.channel,
            .key = source.key,
            .velocity = source.velocity,
            .control = source.control,
            .value = source.value,
            .program = source.program,
            .pitch = source.pitch,
            .pressure = source.pressure,
            .machine_control_command = source.machine_control_command,
        },
    });
    return 0;
}

} // namespace fake_midi_input
