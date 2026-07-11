#include <fstream>
#include <astrial/detail.hpp>

#include <asio.hpp>

#ifdef _WIN32
#include <winbase.h>
#include <windows.h>
#elif defined(__linux__)
#include <sys/ioctl.h>
#include <linux/serial.h>
#endif

namespace fs = std::filesystem;

namespace detail
{
#if defined(__linux__) || defined(__APPLE__)
    std::string read_sys_file(const fs::path& path)
    {
        if (!fs::exists(path)) return "";
        std::ifstream file(path);
        std::string value;
        if (std::getline(file, value))
        {
            return value;
        }
        return "";
    }

    tl::expected<uint16_t, SerialError> parse_hex(const std::string& str)
    {
        if (str.empty()) return tl::make_unexpected(SerialError::ParseError);
        try
        {
            auto value = std::stoul(str, nullptr, 16);
            if (value > UINT16_MAX) return tl::make_unexpected(SerialError::ValueOutOfRange);
            return static_cast<uint16_t>(value);
        }
        catch (const std::invalid_argument&)
        {
            return tl::make_unexpected(SerialError::ParseError);
        }
        catch (const std::out_of_range&)
        {
            return tl::make_unexpected(SerialError::ValueOutOfRange);
        }
    }
#endif

    tl::expected<void, std::error_code> try_open(asio::serial_port& port, std::string_view name)
    {
        asio::error_code ec;
        port.open(std::string(name), ec);
        if (ec) return tl::make_unexpected(ec);

        // try optimizing performance
#ifdef _WIN32
        ::SetupComm(port.native_handle(), 65536, 65536);
#elif defined(__linux__)
        int fd = port.native_handle();
        serial_struct serial_opts{};
        if (::ioctl(fd, TIOCGSERIAL, &serial_opts) == 0)
        {
            serial_opts.flags |= ASYNC_LOW_LATENCY;
            ::ioctl(fd, TIOCSSERIAL, &serial_opts);
        }
#endif

        return {};
    }

    tl::expected<void, std::error_code> set_baud_rate(asio::serial_port& port, uint32_t rate)
    {
        asio::error_code ec;
        port.set_option(asio::serial_port_base::baud_rate(rate), ec);
        if (ec) return tl::make_unexpected(ec);
        return {};
    }

    tl::expected<void, std::error_code> set_parity(asio::serial_port& port, Parity parity)
    {
        asio::serial_port_base::parity::type asio_parity;
        switch (parity)
        {
        case Parity::Even: asio_parity = asio::serial_port_base::parity::even;
            break;
        case Parity::Odd: asio_parity = asio::serial_port_base::parity::odd;
            break;
        default: asio_parity = asio::serial_port_base::parity::none;
            break;
        }

        asio::error_code ec;
        port.set_option(asio::serial_port_base::parity(asio_parity), ec);
        if (ec) return tl::make_unexpected(ec);
        return {};
    }

    tl::expected<void, std::error_code> set_stop_bits(asio::serial_port& port, StopBits stop_bits)
    {
        asio::serial_port_base::stop_bits::type asio_stop;
        switch (stop_bits)
        {
        case StopBits::OnePointFive: asio_stop = asio::serial_port_base::stop_bits::onepointfive;
            break;
        case StopBits::Two: asio_stop = asio::serial_port_base::stop_bits::two;
            break;
        default: asio_stop = asio::serial_port_base::stop_bits::one;
            break;
        }

        asio::error_code ec;
        port.set_option(asio::serial_port_base::stop_bits(asio_stop), ec);
        if (ec) return tl::make_unexpected(ec);
        return {};
    }

    tl::expected<void, std::error_code> set_data_bits(asio::serial_port& port, DataBits data_bits)
    {
        asio::error_code ec;
        port.set_option(asio::serial_port_base::character_size(static_cast<unsigned int>(data_bits)), ec);
        if (ec) return tl::make_unexpected(ec);
        return {};
    }

    tl::expected<void, std::error_code> try_configure_port(asio::serial_port& port, std::string_view name,
                                                           uint32_t rate, Parity parity,
                                                           StopBits stop_bits, DataBits data_bits)
    {
        return try_open(port, name)
              .and_then([&] { return set_baud_rate(port, rate); })
              .and_then([&] { return set_parity(port, parity); })
              .and_then([&] { return set_stop_bits(port, stop_bits); })
              .and_then([&] { return set_data_bits(port, data_bits); });
    }
}
