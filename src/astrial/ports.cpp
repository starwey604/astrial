#include <string>
#include <astrial/port.hpp>
#include <astrial/Serial.hpp>

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
// DEVPKEY_Device_BusReportedDeviceDesc  {540B947E-8B40-45BC-A8A2-6A0B894CBDA2}, 4
static const DEVPROPKEY s_BusReportedDesc = { {0x540b947e,0x8b40,0x45bc,{0xa8,0xa2,0x6a,0x0b,0x89,0x4c,0xbd,0xa2}}, 4 };

static std::string from_wstring(const wchar_t* src, int len = -1)
{
    if (!src || (len == 0)) return {};
    int needed = WideCharToMultiByte(CP_ACP, 0, src, len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(needed, 0);
    WideCharToMultiByte(CP_ACP, 0, src, len, result.data(), needed, nullptr, nullptr);
    if (needed > 0 && result.back() == 0) result.pop_back();
    while (!result.empty() && (result.back() == ' ' || result.back() == '\t' || result.back() == '\0')) result.pop_back();
    return result;
}
#endif

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
#elif defined(_WIN32)
    const GUID guid = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    // GUID_DEVCLASS_PORTS
    HDEVINFO device_info_set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT);
    if (device_info_set == INVALID_HANDLE_VALUE)
    {
        return ports;
    }

    SP_DEVINFO_DATA dev_info_data = {};
    dev_info_data.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(device_info_set, i, &dev_info_data); ++i)
    {
        SerialPortInfo info;

        // Get COMx Port
        HKEY hkey = SetupDiOpenDevRegKey(device_info_set, &dev_info_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (hkey != INVALID_HANDLE_VALUE)
        {
            wchar_t port_name_buf[256] = {};
            DWORD value_size = sizeof(port_name_buf);
            DWORD type = 0;
            if (RegQueryValueExW(hkey, L"PortName", nullptr, &type, reinterpret_cast<LPBYTE>(port_name_buf),
                                 &value_size) == ERROR_SUCCESS)
            {
                info.port_name = from_wstring(port_name_buf);
            }
            RegCloseKey(hkey);
        }

        if (info.port_name.empty()) continue;

        // try bus-reported device desc first (iProduct from USB descriptor)
        {
            DEVPROPTYPE prop_type;
            wchar_t desc_buf[256] = {};
            if (SetupDiGetDevicePropertyW(device_info_set, &dev_info_data, &s_BusReportedDesc,
                                          &prop_type, reinterpret_cast<PBYTE>(desc_buf), sizeof(desc_buf), nullptr, 0))
            {
                info.description = from_wstring(desc_buf);
            }

            if (info.description.empty())
            {
                if (SetupDiGetDeviceRegistryPropertyW(device_info_set, &dev_info_data, SPDRP_DEVICEDESC, nullptr,
                                                      reinterpret_cast<PBYTE>(desc_buf), sizeof(desc_buf), nullptr))
                {
                    info.description = from_wstring(desc_buf);
                }
                else
                {
                    wchar_t friendly_name[256] = {};
                    if (SetupDiGetDeviceRegistryPropertyW(device_info_set, &dev_info_data, SPDRP_FRIENDLYNAME, nullptr,
                                                          reinterpret_cast<PBYTE>(friendly_name),
                                                          sizeof(friendly_name), nullptr))
                    {
                        info.description = from_wstring(friendly_name);
                    }
                    else
                    {
                        info.description = "USB Serial Port";
                    }
                }
            }
        }

        // manufacturer
        wchar_t mfg_buf[256] = {};
        if (SetupDiGetDeviceRegistryPropertyW(device_info_set, &dev_info_data, SPDRP_MFG, nullptr,
                                              reinterpret_cast<PBYTE>(mfg_buf), sizeof(mfg_buf), nullptr))
        {
            info.manufacturer = from_wstring(mfg_buf);
        }

        // extract VID/PID from current device instance ID
        wchar_t instance_id[512] = {};
        if (SetupDiGetDeviceInstanceIdW(device_info_set, &dev_info_data, instance_id,
                                        sizeof(instance_id) / sizeof(wchar_t), nullptr))
        {
            std::string id_str = from_wstring(instance_id);

            size_t vid_pos = id_str.find("VID_");
            size_t pid_pos = id_str.find("PID_");
            if (vid_pos != std::string::npos && pid_pos != std::string::npos)
            {
                try
                {
                    info.vendor_id = static_cast<uint16_t>(std::stoul(id_str.substr(vid_pos + 4, 4), nullptr, 16));
                    info.product_id = static_cast<uint16_t>(std::stoul(id_str.substr(pid_pos + 4, 4), nullptr, 16));
                }
                catch (...)
                {
                }
            }
        }

        // walk up the device tree to find parent USB node for serial number and better description
        DEVINST dev = dev_info_data.DevInst;
        for (int level = 0; level < 4; ++level)
        {
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, dev, 0) != CR_SUCCESS) break;
            dev = parent;

            WCHAR parent_id[512] = {};
            if (CM_Get_Device_IDW(dev, parent_id, 512, 0) != CR_SUCCESS) continue;
            std::string pid_str = from_wstring(parent_id);

            if (pid_str.find("VID_") == std::string::npos) continue;

            // found the USB device node — try to get the bus-reported description (iProduct)
            HDEVINFO parent_info = SetupDiCreateDeviceInfoList(nullptr, nullptr);
            if (parent_info != INVALID_HANDLE_VALUE)
            {
                SP_DEVINFO_DATA parent_data = {};
                parent_data.cbSize = sizeof(SP_DEVINFO_DATA);
                if (SetupDiOpenDeviceInfoW(parent_info, parent_id, nullptr, 0, &parent_data))
                {
                    DEVPROPTYPE prop_type;
                    wchar_t parent_desc[256] = {};
                    if (SetupDiGetDevicePropertyW(parent_info, &parent_data, &s_BusReportedDesc,
                                                  &prop_type, reinterpret_cast<PBYTE>(parent_desc), sizeof(parent_desc),
                                                  nullptr, 0))
                    {
                        info.description = from_wstring(parent_desc);
                    }
                    else
                    {
                        wchar_t fallback[256] = {};
                        if (SetupDiGetDeviceRegistryPropertyW(parent_info, &parent_data, SPDRP_DEVICEDESC, nullptr,
                                                              reinterpret_cast<PBYTE>(fallback), sizeof(fallback), nullptr))
                        {
                            info.description = from_wstring(fallback);
                        }
                    }
                }
                SetupDiDestroyDeviceInfoList(parent_info);
            }

            // extract serial number from parent USB instance ID
            size_t last_slash = pid_str.find_last_of('\\');
            if (last_slash != std::string::npos && last_slash < pid_str.length() - 1)
            {
                std::string sn = pid_str.substr(last_slash + 1);
                if (sn.find('&') == std::string::npos)
                {
                    info.serial_number = sn;
                }
            }
            break;
        }

        ports.push_back(info);
    }
    SetupDiDestroyDeviceInfoList(device_info_set);
#endif
    return ports;
}
