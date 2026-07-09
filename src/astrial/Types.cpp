#include <astrial/Types.hpp>

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


