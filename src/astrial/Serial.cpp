#include <astrial/Serial.hpp>
#include <astrial/SerialBuilder.hpp>

#include <asio.hpp>
#include <thread>

#ifdef _WIN32
#include <winbase.h>
#endif

class Serial::Impl
{
public:
    asio::io_context m_ctx;
    asio::serial_port m_port;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> m_work_guard;
    std::jthread m_thread;
    std::array<uint8_t, ASTRIAL_BUFFER_LENGTH> m_rx_buffer{};
    std::function<void(std::span<const uint8_t>)> m_callback;
    bool m_read_started = false;

    Impl() : m_ctx(1), m_port(m_ctx)
    {
    };

    ~Impl()
    {
        close();
    }

    void close()
    {
        asio::error_code ec;
        if (m_port.is_open()) m_port.close(ec);
        m_work_guard.reset();
        m_ctx.stop();
        if (m_thread.joinable()) m_thread.join();
    }

    void start_read()
    {
        if (m_read_started) return;
        m_read_started = true;

        struct Reader
        {
            Impl& impl_ref;

            void operator()(const asio::error_code& ec, std::size_t bytes)
            {
                if (!ec && bytes > 0)
                {
                    impl_ref.m_callback(std::span<const uint8_t>(impl_ref.m_rx_buffer.data(), bytes));
                }
                impl_ref.m_port.async_read_some(asio::buffer(impl_ref.m_rx_buffer), *this);
            }
        };
        m_port.async_read_some(asio::buffer(m_rx_buffer), Reader{*this});
    }
};

tl::expected<Serial, std::error_code> SerialBuilder::open(const std::string_view name) const
{
    Serial serial;
    auto& impl = *serial.m_impl;

    asio::error_code ec;

    // try open
    impl.m_port.open(std::string(name), ec);
    if (ec) return tl::make_unexpected(ec);

#ifdef _WIN32
    ::SetupComm(impl.m_port.native_handle(), 65536, 65536);
#endif

    // try set baud rate
    impl.m_port.set_option(asio::serial_port_base::baud_rate(m_rate), ec);
    if (ec) return tl::make_unexpected(ec);

    // parity
    asio::serial_port_base::parity::type asio_parity;
    switch (m_parity)
    {
    case Parity::Even: asio_parity = asio::serial_port_base::parity::even;
        break;
    case Parity::Odd: asio_parity = asio::serial_port_base::parity::odd;
        break;
    default: asio_parity = asio::serial_port_base::parity::none;
        break;
    }

    impl.m_port.set_option(asio::serial_port_base::parity(asio_parity), ec);
    if (ec) return tl::make_unexpected(ec);

    // background thread setup
    impl.m_work_guard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
        impl.m_ctx.get_executor()
    );
    impl.m_thread = std::jthread([&impl]
    {
#ifdef _WIN32
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        impl.m_ctx.run();
    });

    return std::move(serial);
}

tl::expected<Serial, std::error_code> SerialBuilder::open(const SerialPortInfo& info) const
{
    return open(info.port_name);
}

SerialBuilder Serial::builder()
{
    return SerialBuilder{};
}


Serial::~Serial() = default;
Serial::Serial(Serial&&) noexcept = default;
Serial& Serial::operator=(Serial&&) noexcept = default;

void Serial::on_data(std::function<void(std::span<const uint8_t>)> callback)
{
    if (callback)
    {
        m_impl->m_callback = std::move(callback);
        m_impl->start_read();
    }
}

tl::expected<void, std::error_code> Serial::write(const std::span<const uint8_t> data)
{
    if (!m_impl->m_port.is_open()) return tl::make_unexpected(SerialError::DeviceDisconnected);
    asio::error_code ec;
    asio::write(m_impl->m_port, asio::buffer(data.data(), data.size()), ec);
    if (ec) return tl::make_unexpected(ec);
    return {};
}

void Serial::close()
{
    m_impl->close();
}

Serial::Serial() : m_impl(std::make_unique<Impl>())
{
}
