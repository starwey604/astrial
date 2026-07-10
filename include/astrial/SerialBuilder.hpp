#ifndef ASTRIAL_SERIALBUILDER_HPP
#define ASTRIAL_SERIALBUILDER_HPP

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
    SerialBuilder& buad_rate(uint32_t rate);
    SerialBuilder& parity(Parity parity);
    SerialBuilder& stop_bits(StopBits stop_bits);
    SerialBuilder& data_bits(DataBits data_bits);

    tl::expected<Serial, std::error_code> open(std::string_view name) const;
    tl::expected<Serial, std::error_code> open(const SerialPortInfo& info) const;

private:
    uint32_t m_rate{};
    Parity m_parity{};
    StopBits m_stop_bits{};
    DataBits m_data_bits{DataBits::Eight};
};

#endif //ASTRIAL_SERIALBUILDER_HPP
