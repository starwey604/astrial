#ifndef ASTRIAL_TYPES_HPP
#define ASTRIAL_TYPES_HPP

#include <system_error>
#include <cstdint>
#include <iosfwd>
#include <string>

enum class Parity { None, Odd, Even, Mark, Space };

enum class StopBits { One, OnePointFive, Two };

enum class DataBits { Five = 5, Six = 6, Seven = 7, Eight = 8 };

enum class FlowControl { None, Software, Hardware };

// Logical asserted states, not TTL voltage levels. Values match QSerialPort's
// corresponding DB9 signal flags; no Qt dependency is required.
enum class PinoutSignals : uint16_t
{
    None = 0,
    DataTerminalReady = 0x04,
    DataCarrierDetect = 0x08,
    DataSetReady = 0x10,
    RequestToSend = 0x20,
    RingIndicator = 0x40,
    ClearToSend = 0x80
};

constexpr PinoutSignals operator|(PinoutSignals a, PinoutSignals b) noexcept
{
    return static_cast<PinoutSignals>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
constexpr PinoutSignals operator&(PinoutSignals a, PinoutSignals b) noexcept
{
    return static_cast<PinoutSignals>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
constexpr PinoutSignals& operator|=(PinoutSignals& a, PinoutSignals b) noexcept
{
    return a = a | b;
}
constexpr bool has_signal(PinoutSignals signals, PinoutSignals signal) noexcept
{
    return signal != PinoutSignals::None && (signals & signal) == signal;
}

// Driver-maintained cumulative counters for this open device. They can wrap
// or reset when a device is reopened. USB drivers may count error reports,
// rather than individual corrupt bytes. Currently available on Linux only.
struct SerialLineErrorCounters
{
    uint32_t parity{};
    uint32_t framing{};
    uint32_t overrun{};
    uint32_t breaks{};
    uint32_t buffer_overrun{};
};

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
    UnknownError,
    UnsupportedOperation
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
