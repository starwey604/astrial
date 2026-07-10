#include <astrial/SerialBuilder.hpp>
#include <astrial/Serial.hpp>

#include "asio/error_code.hpp"

SerialBuilder& SerialBuilder::buad_rate(const uint32_t rate)
{
    m_rate = rate;
    return *this;
}

SerialBuilder& SerialBuilder::parity(const Parity parity)
{
    m_parity = parity;
    return *this;
}

SerialBuilder& SerialBuilder::stop_bits(const StopBits stop_bits)
{
    m_stop_bits = stop_bits;
    return *this;
}

SerialBuilder& SerialBuilder::data_bits(const DataBits data_bits)
{
    m_data_bits = data_bits;
    return *this;
}
