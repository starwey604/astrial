#ifndef ASTRIAL_DETAIL_HPP
#define ASTRIAL_DETAIL_HPP

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "Types.hpp"
#include <tl/expected.hpp>
#include <asio/serial_port.hpp>
#include <asio/serial_port_base.hpp>

class SerialBuilder;

namespace detail
{
#if defined(__linux__) || defined(__APPLE__)
    std::string read_sys_file(const std::filesystem::path& path);
    tl::expected<uint16_t, SerialError> parse_hex(const std::string& str);
#endif

    tl::expected<void, std::error_code> try_open(asio::serial_port& port, std::string_view name);
    tl::expected<void, std::error_code> set_baud_rate(asio::serial_port& port, uint32_t rate);
    tl::expected<void, std::error_code> set_parity(asio::serial_port& port, Parity parity);
    tl::expected<void, std::error_code> set_stop_bits(asio::serial_port& port, StopBits stop_bits);
    tl::expected<void, std::error_code> set_data_bits(asio::serial_port& port, DataBits data_bits);
    tl::expected<void, std::error_code> try_configure_port(asio::serial_port& port, std::string_view name,
                                                           uint32_t rate, Parity parity,
                                                           StopBits stop_bits, DataBits data_bits);
}

#endif //ASTRIAL_DETAIL_HPP
