#ifndef ASTRIAL_SERIAL_HPP
#define ASTRIAL_SERIAL_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <system_error>
#include <vector>

#include <tl/expected.hpp>

#include "SerialBuilder.hpp"

class Serial
{
public:
    friend class SerialBuilder;

    static SerialBuilder builder();
    static std::vector<SerialInfo> list_ports();

    ~Serial();

    Serial(const Serial&) = delete;
    Serial& operator=(const Serial&) = delete;
    Serial(Serial&&) noexcept;
    Serial& operator=(Serial&&) noexcept;

    void on_data(std::function<void(std::span<const uint8_t>)> callback);

    using ReadBufferProvider = std::function<std::span<uint8_t>()>;
    using ReadCallback = std::function<void(const std::error_code&, std::size_t)>;

    // Install a continuous zero-copy read loop. Both callbacks run on
    // Astrial's I/O thread. The provider lends one mutable buffer and the
    // completion callback returns it after the asynchronous operation ends.
    // Returning an empty span pauses reads until resume_read() is called.
    void on_data_borrowed(ReadBufferProvider provider, ReadCallback callback);
    void resume_read();

    tl::expected<void, std::error_code> write(std::span<const uint8_t> data);
    tl::expected<void, std::error_code> write(std::span<const uint8_t> data, std::chrono::milliseconds timeout);

    using WriteCallback = std::function<void(const std::error_code&, std::size_t)>;
    void async_write(std::span<const uint8_t> data, WriteCallback callback);

    // The caller retains ownership and must keep data unchanged until the
    // callback runs. Completion is reported for success, cancellation, and
    // disconnection after the request has been accepted.
    void async_write_borrowed(std::span<const uint8_t> data, WriteCallback callback);
    void close();

    void on_disconnect(std::function<void(const std::error_code&)> callback);
    void on_reconnect(std::function<void()> callback);

private:
    Serial();
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif //ASTRIAL_SERIAL_HPP
