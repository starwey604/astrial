#include <astrial/Types.hpp>
#include <ostream>

const char* SerialErrorCategory::name() const noexcept
{
    return "SerialErrorCategory";
}

std::string SerialErrorCategory::message(int ev) const
{
    switch (static_cast<SerialError>(ev))
    {
    case SerialError::Success: return "Success";
    case SerialError::PortNotFound: return "PortNotFound";
    case SerialError::PermissionDenied: return "PermissionDenied";
    case SerialError::InvalidArgument: return "InvalidArgument";
    case SerialError::DeviceDisconnected: return "DeviceDisconnected";
    case SerialError::ParseError: return "ParseError";
    case SerialError::ValueOutOfRange: return "ValueOutOfRange";
    default: return "UnknownError";
    }
}

inline const std::error_category& get_serial_category()
{
    static SerialErrorCategory instance_;
    return instance_;
};

std::error_code make_error_code(SerialError e)
{
    return std::error_code(static_cast<int>(e), get_serial_category());
}

std::ostream& operator<<(std::ostream& os, const SerialPortInfo& info)
{
    os << "SerialPortInfo{";
    os << "port_name: \"" << info.port_name << "\", ";
    os << "description: \"" << info.description << "\", ";
    os << "vendor_id: 0x" << std::hex << info.vendor_id << std::dec << ", ";
    os << "product_id: 0x" << std::hex << info.product_id << std::dec << ", ";
    os << "serial_number: \"" << info.serial_number << "\", ";
    os << "manufacturer: \"" << info.manufacturer << "\"}";
    return os;
}


