#ifndef ASTRIAL_TYPES_HPP
#define ASTRIAL_TYPES_HPP

#include <system_error>
#include <cstdint>
#include <iosfwd>

enum class Parity { None, Odd, Even };

enum class StopBits { One, OnePointFive, Two };

struct SerialPortInfo
{
    std::string port_name;
    std::string description;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    std::string serial_number; // 硬件唯一序列号
    std::string manufacturer;
};

std::ostream& operator<<(std::ostream& os, const SerialPortInfo& info);


enum class SerialError
{
    Success = 0,
    PortNotFound, // 串口不存在
    PermissionDenied, // 无权限（可能被其他程序占用）
    InvalidArgument, // 参数错误（如不支持的波特率）
    DeviceDisconnected, // 设备意外断开
    ParseError, // 字符串解析失败（如无效的十六进制字符串）
    ValueOutOfRange, // 值超出范围
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
