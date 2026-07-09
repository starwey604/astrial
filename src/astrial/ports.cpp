#include <astrial/port.hpp>
#include <astrial/Serial.hpp>

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
}

std::vector<SerialPortInfo> Serial::list_ports()
{
    std::vector<SerialPortInfo> ports;
#if defined(__linux__) || defined(__APPLE__)
    try
    {
        fs::path tty_dir("/sys/class/tty");
        if (fs::exists(tty_dir))
        {
            for (const auto& entry : fs::directory_iterator(tty_dir))
            {
                auto name = entry.path().filename().string();
                fs::path device_link = entry.path() / "device";

                // rule 1: must have "device" link
                if (!fs::exists(device_link) || !fs::is_symlink(device_link))
                {
                    continue;
                }

                // rule 2: exclude virtual type
                fs::path real_path = fs::read_symlink(device_link);
                auto real_path_str = real_path.string();

                if (real_path_str.find("virtual") != std::string::npos)
                {
                    continue;
                }

                auto port_path = "/dev/" + name;

                SerialPortInfo info;
                info.port_name = port_path;
                info.description = "Astrial Serial Port";

                fs::path sys_device_node = fs::canonical(device_link);
                fs::path usb_node = sys_device_node;
                bool found_usb = false;

                while (usb_node != usb_node.root_path())
                {
                    if (fs::exists(usb_node / "idVendor"))
                    {
                        found_usb = true;
                        break;
                    }
                    usb_node = usb_node.parent_path();
                }

                if (found_usb)
                {
                    info.vendor_id = detail::parse_hex(detail::read_sys_file(usb_node / "idVendor")).value();
                    info.product_id = detail::parse_hex(detail::read_sys_file(usb_node / "idProduct")).value();
                    info.serial_number = detail::read_sys_file(usb_node / "serial");
                    info.manufacturer = detail::read_sys_file(usb_node / "manufacturer");
                    info.description = "USB Serial Device";
                }
                else
                {
                    info.description = "On-board UART (" + name + ")";
                }
                ports.push_back(info);
            }
        }
    }
    catch (...)
    {
    };
    // TODO: Windows implementation
#elif defined(_WIN32)
#endif
    return ports;
}
