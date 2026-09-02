#ifndef ASTRIAL_TYPES_HPP
#define ASTRIAL_TYPES_HPP

#include <system_error>
#include <cstdint>
#include <iosfwd>

enum class Parity { None, Odd, Even };

enum class StopBits { One, OnePointFive, Two };

enum class DataBits { Five = 5, Six = 6, Seven = 7, Eight = 8 };

struct SerialInfo
{
    std::string port_name;
    std::string description;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    std::string serial_number; // Stable hardware serial number
    std::string manufacturer;
};

enum class SerialState { Disconnected, Connected, Reconnecting };

std::ostream& operator<<(std::ostream& os, const SerialInfo& info);


enum class SerialError
{
    Success = 0,
    PortNotFound,
    PermissionDenied, // The port may already be in use.
    InvalidArgument, // For example, an unsupported baud rate.
    DeviceDisconnected,
    ParseError,
    ValueOutOfRange,
    WriteTimeout,
    UnknownError
};

class SerialErrorCategory : public std::error_category
{
public:
    const char* name() const noexcept override;
    std::string message(int ev) const override;
};

const std::error_category& get_serial_category();

std::error_code make_error_code(SerialError e);

template <>
struct std::is_error_code_enum<SerialError> : std::true_type
{
};

#endif //ASTRIAL_TYPES_HPP
