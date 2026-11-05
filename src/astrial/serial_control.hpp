#ifndef ASTRIAL_SERIAL_CONTROL_HPP
#define ASTRIAL_SERIAL_CONTROL_HPP

#include <astrial/Types.hpp>
#include <asio/serial_port.hpp>
#include <tl/expected.hpp>

namespace detail
{
tl::expected<void, std::error_code> set_modem_output(asio::serial_port& port,
                                                     bool dtr, bool asserted);
tl::expected<PinoutSignals, std::error_code> read_pinout(asio::serial_port& port);
tl::expected<void, std::error_code> set_break(asio::serial_port& port, bool enabled);
tl::expected<SerialLineErrorCounters, std::error_code> read_line_errors(asio::serial_port& port);
tl::expected<void, std::error_code> set_native_parity(asio::serial_port& port, Parity parity);
}

#endif
