#ifndef ASTRIAL_SERIALBUILDER_HPP
#define ASTRIAL_SERIALBUILDER_HPP

#include <chrono>
#include <cstdint>
#include <string_view>
#include <system_error>

#include "Types.hpp"
#include <tl/expected.hpp>

class Serial;

class SerialBuilder
{
public:
    SerialBuilder() = default;
    SerialBuilder& baud_rate(uint32_t rate);
    SerialBuilder& parity(Parity parity);
    SerialBuilder& stop_bits(StopBits stop_bits);
    SerialBuilder& data_bits(DataBits data_bits);
    SerialBuilder& auto_reconnect(bool enable, std::chrono::milliseconds interval = std::chrono::seconds(2));

    tl::expected<Serial, std::error_code> open(std::string_view name) const;
    tl::expected<Serial, std::error_code> open(const SerialInfo& info) const;

private:
    uint32_t m_rate{};
    Parity m_parity{};
    StopBits m_stop_bits{};
    DataBits m_data_bits{DataBits::Eight};
    bool m_auto_reconnect = true;
    std::chrono::milliseconds m_reconnect_interval = std::chrono::seconds(2);
};

#endif //ASTRIAL_SERIALBUILDER_HPP
