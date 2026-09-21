// Serial hardware-in-the-loop harness.
//
// This executable is built and registered with CTest only when
// ASTRIAL_BUILD_HIL_TESTS=ON. It collects the checks that cannot be exercised
// on a pseudo-terminal and therefore need a real serial fixture:
//
//   * DTR/RTS electrical level control and CTS/DSR/DCD/RI reads
//   * hardware (RTS/CTS) flow control actually throttling the peer
//   * break condition generation and detection
//   * RS-485 half-duplex direction control (manual RTS and TIOCSRS485)
//   * line error counters (framing/parity/overrun) via TIOCGICOUNT
//   * ASYNC_LOW_LATENCY applicability on the host UART
//
// Nothing here is implemented yet; the harness reports the planned checks and
// skips (exit 77). Provide ASTRIAL_HIL_PORT=<device> when the fixture exists.
#include <astrial.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
constexpr int kSkipReturnCode = 77;

void report(const char* check, const char* status)
{
    std::cout << "  - " << check << ": " << status << '\n';
}
}

int main()
{
    std::cout << "Astrial serial hardware-in-the-loop harness\n";

    const char* port = std::getenv("ASTRIAL_HIL_PORT");
    if (port == nullptr || *port == '\0')
    {
        std::cout << "ASTRIAL_HIL_PORT is not set; skipping hardware checks\n";
        return kSkipReturnCode;
    }

    std::cout << "DUT port: " << port << '\n';
    std::cout << "Hardware checks (not implemented yet):\n";
    report("DTR/RTS level control and CTS/DSR/DCD/RI reads", "pending");
    report("hardware RTS/CTS flow control throttling", "pending");
    report("break generation and detection", "pending");
    report("RS-485 half-duplex direction control", "pending");
    report("line error counters (TIOCGICOUNT)", "pending");
    report("ASYNC_LOW_LATENCY applicability", "pending");

    std::cout << "Serial HIL checks are not implemented yet; skipping\n";
    return kSkipReturnCode;
}
