#ifndef ASTRIAL_TYPES_HPP
#define ASTRIAL_TYPES_HPP
#include <system_error>

enum class Parity { None, Odd, Even };

enum class StopBits { One, OnePointFive, Two };

enum class SerialError
{
    Success = 0,
    PortNotFound, // 串口不存在
    PermissionDenied, // 无权限（可能被其他程序占用）
    InvalidArgument, // 参数错误（如不支持的波特率）
    DeviceDisconnected, // 设备意外断开
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
