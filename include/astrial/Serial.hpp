#ifndef ASTRIAL_SERIAL_HPP
#define ASTRIAL_SERIAL_HPP

#include <memory>
#include <span>

#include <tl/expected.hpp>

#include "SerialBuilder.hpp"

class Serial
{
public:
    friend class SerialBuilder;

    static SerialBuilder builder();
    static std::vector<SerialPortInfo> list_ports();

    ~Serial();

    Serial(const Serial&) = delete;
    Serial& operator=(const Serial&) = delete;
    Serial(Serial&&) noexcept;
    Serial& operator=(Serial&&) noexcept;

    void on_data(std::function<void(std::span<const uint8_t>)> callback);
    tl::expected<void, std::error_code> write(std::span<const uint8_t> data);
    void close();

private:
    Serial();
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif //ASTRIAL_SERIAL_HPP
