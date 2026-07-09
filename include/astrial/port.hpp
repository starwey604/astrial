#ifndef ASTRIAL_PORT_HPP
#define ASTRIAL_PORT_HPP
#include "Types.hpp"
#include "tl/expected.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
#endif

namespace detail
{
#if defined(__linux__) || defined(__APPLE__)
    // 读取 Linux /sys 节点下的单行文本文件
    static std::string read_sys_file(const fs::path& path);
    // 解析 16 进制字符串（如 "1a86" -> 0x1A86）
    static tl::expected<uint16_t, SerialError> parse_hex(const std::string& str);
#endif
}

#endif //ASTRIAL_PORT_HPP
