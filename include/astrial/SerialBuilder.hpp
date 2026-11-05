#ifndef ASTRIAL_SERIALBUILDER_HPP
#define ASTRIAL_SERIALBUILDER_HPP

#include <chrono>
#include <cstdint>
#include <string_view>
#include <optional>
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
    SerialBuilder& flow_control(FlowControl flow_control);
    // Applied after opening; opening itself may briefly assert driver defaults.
    // Explicit request_to_send is incompatible with hardware flow control.
    SerialBuilder& data_terminal_ready(bool asserted);
    SerialBuilder& request_to_send(bool asserted);
    SerialBuilder& auto_reconnect(bool enable, std::chrono::milliseconds interval = std::chrono::seconds(2));

    tl::expected<Serial, std::error_code> open(std::string_view name) const;
    tl::expected<Serial, std::error_code> open(const SerialInfo& info) const;

private:
    uint32_t m_rate{};
    Parity m_parity{};
    StopBits m_stop_bits{};
    DataBits m_data_bits{DataBits::Eight};
    FlowControl m_flow_control{FlowControl::None};
    std::optional<bool> m_dtr;
    std::optional<bool> m_rts;
    bool m_auto_reconnect = true;
    std::chrono::milliseconds m_reconnect_interval = std::chrono::seconds(2);
};

#endif //ASTRIAL_SERIALBUILDER_HPP
