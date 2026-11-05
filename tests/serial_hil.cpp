// Opt-in electrical RS-232 loopback test. Never select a port automatically.
// Required DB9 wiring, disconnected from the NXP board or other equipment:
//   2 <-> 3, 7 <-> 8, and 4 <-> 1/6/9.
// ASTRIAL_HIL_PORT=<port> ASTRIAL_HIL_FIXTURE=rs232-loopback
// Other HIL checks require an independently controlled peer/instrument.
#include <astrial.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
void check(tl::expected<void, std::error_code> result, const char* operation)
{
    if (!result) throw std::system_error(result.error(), operation);
}
void wait_signals(Serial& serial, bool dtr, bool rts)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do
    {
        auto result = serial.pinout_signals();
        if (!result) throw std::system_error(result.error(), "pinout_signals");
        const auto pins = *result;
        if (has_signal(pins, PinoutSignals::DataTerminalReady) == dtr &&
            has_signal(pins, PinoutSignals::DataSetReady) == dtr &&
            has_signal(pins, PinoutSignals::DataCarrierDetect) == dtr &&
            has_signal(pins, PinoutSignals::RingIndicator) == dtr &&
            has_signal(pins, PinoutSignals::RequestToSend) == rts &&
            has_signal(pins, PinoutSignals::ClearToSend) == rts) return;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("DB9 loopback state mismatch: check all control-line wiring");
}
}

int main()
{
    const char* port = std::getenv("ASTRIAL_HIL_PORT");
    const char* fixture = std::getenv("ASTRIAL_HIL_FIXTURE");
    if (!port || !*port || !fixture || std::string(fixture) != "rs232-loopback")
    {
        std::cout << "Skipping: set ASTRIAL_HIL_PORT and ASTRIAL_HIL_FIXTURE=rs232-loopback\n"
                  << "DB9 loopback only: 2-3, 7-8, 4-1-6-9. Disconnect other equipment.\n";
        return 77;
    }
    try
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<uint8_t> received;
        std::error_code disconnect_error;
        auto opened = Serial::builder().baud_rate(115200).flow_control(FlowControl::None)
                          .auto_reconnect(false).open(port);
        if (!opened) throw std::system_error(opened.error(), "open HIL port");
        auto serial = std::move(*opened);
        // Destroy/close serial before callback storage, including failure paths.
        struct CloseGuard { Serial& serial; ~CloseGuard() { serial.close(); } } close{serial};
        const auto initial = serial.pinout_signals();
        if (!initial) throw std::system_error(initial.error(), "initial pinout");
        struct RestoreOutputs
        {
            Serial& serial;
            PinoutSignals initial;
            ~RestoreOutputs()
            {
                (void)serial.set_data_terminal_ready(has_signal(initial, PinoutSignals::DataTerminalReady));
                (void)serial.set_request_to_send(has_signal(initial, PinoutSignals::RequestToSend));
            }
        } restore{serial, *initial};

        for (bool dtr : {false, true})
            for (bool rts : {false, true})
            {
                check(serial.set_data_terminal_ready(dtr), "set DTR");
                check(serial.set_request_to_send(rts), "set RTS");
                wait_signals(serial, dtr, rts);
            }
        std::cout << "PASS: DTR->DSR/DCD/RI and RTS->CTS, all four output combinations\n";

        serial.on_data([&](std::span<const uint8_t> bytes)
        {
            std::lock_guard lock(mutex);
            received.insert(received.end(), bytes.begin(), bytes.end());
            cv.notify_all();
        });
        serial.on_disconnect([&](const std::error_code& error)
        {
            std::lock_guard lock(mutex);
            disconnect_error = error;
            cv.notify_all();
        });
        std::array<uint8_t, 1024> payload{};
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i);
        check(serial.write(payload, 2s), "write loopback payload");
        {
            std::unique_lock lock(mutex);
            require(cv.wait_for(lock, 3s, [&] { return received.size() >= payload.size() || disconnect_error; }),
                    "data loopback timed out");
            require(!disconnect_error, "device disconnected during data loopback");
            require(received == std::vector<uint8_t>(payload.begin(), payload.end()), "loopback data mismatch");
        }
        std::cout << "PASS: 1024-byte binary TX/RX loopback at 115200 8N1\n";
        const auto counters = serial.line_error_counters();
        if (counters)
            std::cout << "INFO: driver exposes line error counters (not error-injection tested)\n";
        else if (counters.error() == SerialError::UnsupportedOperation)
            std::cout << "INFO: driver does not expose line error counters\n";
        else throw std::system_error(counters.error(), "line_error_counters");
        std::cout << "Loopback suite passed. NOT TESTED: independent parity/stop-bit validation,\n"
                  << "RTS/CTS throttling, BREAK waveform/detection, error injection, reconnect,\n"
                  << "RS-485 or precise timing. Those require a peer/instrument fixture.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "RS-232 HIL failed: " << error.what() << '\n';
        return 1;
    }
}
