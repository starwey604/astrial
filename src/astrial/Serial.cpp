#include <astrial/Serial.hpp>
#include <astrial/SerialBuilder.hpp>
#include <astrial/detail.hpp>

#include <asio.hpp>
#include <memory_resource>
#include <readerwriterqueue.h>
#include <queue>
#include <thread>
#include <semaphore>

class Serial::Impl
{
public:
    asio::io_context m_ctx;
    asio::serial_port m_port;
    std::string m_port_name;
    uint32_t m_baud_rate{};
    Parity m_parity{};
    StopBits m_stop_bits{};
    DataBits m_data_bits{DataBits::Eight};
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> m_work_guard;

    asio::steady_timer m_reconnect_timer;

    std::jthread m_thread;
    std::array<uint8_t, ASTRIAL_READ_BUFFER_LENGTH> m_rx_buffer{};

    struct AsyncWriteReq
    {
        std::pmr::vector<uint8_t> buffer;
        WriteCallback callback;
        std::binary_semaphore* signal_sem;
        tl::expected<void, std::error_code>* res;
    };

    moodycamel::ReaderWriterQueue<AsyncWriteReq, ASTRIAL_WRITE_BUFFER_LENGTH> m_write_queue;
    std::pmr::unsynchronized_pool_resource m_pool_resource;

    std::function<void(std::span<const uint8_t>)> m_data_callback;
    std::function<void(const asio::error_code& code)> m_disconnect_callback;
    std::function<void()> m_reconnect_callback;

    bool m_read_started{false};
    bool m_auto_reconnect{true};
    std::chrono::milliseconds m_reconnect_interval{std::chrono::seconds(3)};
    std::atomic<SerialState> m_state{SerialState::Disconnected};
    std::atomic<bool> m_is_closed_by_user{false};

    Impl() : m_ctx(1), m_port(m_ctx), m_reconnect_timer(m_ctx)
    {
    };

    ~Impl()
    {
        close();
    }

    void close()
    {
        asio::error_code ec;

        m_is_closed_by_user.store(true, std::memory_order_release);
        m_reconnect_timer.cancel();

        if (m_port.is_open()) m_port.close(ec);
        m_work_guard.reset();
        m_ctx.stop();
        if (m_thread.joinable()) m_thread.join();

        m_state.store(SerialState::Disconnected, std::memory_order_release);
    }

    void handle_disconnection(const std::error_code& ec)
    {
        // filter error
        if (ec == asio::error::operation_aborted)
        {
            return;
        }

#if defined(_WIN32)
        if (ec.value() == ERROR_DEVICE_REMOVED ||
            ec.value() == ERROR_ACCESS_DENIED ||
            ec.value() == ERROR_GEN_FAILURE)
        {
            return;
        }
#else
        if (ec.value() == ENODEV || ec.value() == EIO || ec.value() == ENXIO)
        {
            return;
        }
#endif

        // shutdown port first
        asio::error_code close_ec;
        m_port.close(close_ec);
        m_read_started = false;
        m_state.store(SerialState::Disconnected, std::memory_order_release);

        // notifying user
        if (m_disconnect_callback)
        {
            m_disconnect_callback(ec);
        }

        // reconnect if needed
        if (m_auto_reconnect && !m_is_closed_by_user.load(std::memory_order_acquire))
        {
            m_state.store(SerialState::Reconnecting, std::memory_order_release);
            schedule_reconnect();
        }
    }

    void schedule_reconnect()
    {
        if (m_is_closed_by_user) return;

        m_reconnect_timer.expires_after(m_reconnect_interval);
        m_reconnect_timer.async_wait([this](const asio::error_code& timer_ec)
        {
            if (timer_ec) // timer was canceled, exiting
            {
                return;
            }

            // try re-opening port
            (void)detail::try_configure_port(m_port, m_port_name, m_baud_rate,
                                             m_parity, m_stop_bits, m_data_bits)
                 .or_else([this](auto&&) { schedule_reconnect(); }) // failed, try later
                 .and_then([this]() -> tl::expected<void, std::error_code>
                  {
                      m_state.store(SerialState::Connected, std::memory_order_release);
                      start_read_loop();
                      // notifying user
                      if (m_reconnect_callback) m_reconnect_callback();
                      return {};
                  });
        });
    }

    void start_read_loop()
    {
        if (m_read_started) return;
        m_read_started = true;

        struct Reader
        {
            Impl& impl;

            void operator()(const asio::error_code& ec, std::size_t bytes)
            {
                if (!ec)
                {
                    if (bytes > 0 && impl.m_data_callback)
                    {
                        impl.m_data_callback(std::span<const uint8_t>(impl.m_rx_buffer.data(), bytes));
                    }
                    impl.m_port.async_read_some(asio::buffer(impl.m_rx_buffer), *this);
                }
                else
                {
                    if (ec == asio::error::operation_aborted && impl.m_is_closed_by_user.
                                                                     load(std::memory_order_acquire))
                    {
                        return;
                    }
                    impl.m_read_started = false;
                    impl.handle_disconnection(ec);
                }
            }
        };
        m_port.async_read_some(asio::buffer(m_rx_buffer), Reader{*this});
    }

    void enqueue_write(const std::span<const uint8_t> data, WriteCallback callback)
    {
        const std::pmr::polymorphic_allocator<uint8_t> alloc(&m_pool_resource);
        std::pmr::vector temp_buf(data.begin(), data.end(), alloc);

        const bool was_empty = m_write_queue.peek() == nullptr;
        m_write_queue.enqueue(AsyncWriteReq{std::move(temp_buf), std::move(callback)});

        if (was_empty)
        {
            asio::post(m_ctx, [this]
            {
                start_write_loop();
            });
        }
    }

    void start_write_loop()
    {
        auto request = m_write_queue.peek();
        if (request == nullptr) return; // empty

        asio::async_write(m_port, asio::buffer(request->buffer.data(), request->buffer.size()),
                          [this, request](const asio::error_code& ec, std::size_t bytes_transferred)
                          {
                              if (request->callback)
                              {
                                  request->callback(ec, bytes_transferred);
                              }
                              if (request->signal_sem) request->signal_sem->release();
                              if (!ec)
                              {
                                  m_write_queue.pop();
                                  start_write_loop();
                              }
                              else
                              {
                                  // hardware error will be filtered inside
                                  handle_disconnection(ec);
                              }
                          }
        );
    }
};

tl::expected<Serial, std::error_code> SerialBuilder::open(const std::string_view name) const
{
    Serial serial;
    auto& impl = *serial.m_impl;

    auto result = detail::try_configure_port(impl.m_port, name, m_rate, m_parity, m_stop_bits, m_data_bits);
    if (!result) return tl::make_unexpected(result.error());

    // other param
    impl.m_port_name = name;
    impl.m_baud_rate = m_rate;
    impl.m_parity = m_parity;
    impl.m_stop_bits = m_stop_bits;
    impl.m_data_bits = m_data_bits;
    impl.m_auto_reconnect = m_auto_reconnect;
    impl.m_reconnect_interval = m_reconnect_interval;

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

    impl.m_state.store(SerialState::Connected, std::memory_order_release);
    return std::move(serial);
}

tl::expected<Serial, std::error_code> SerialBuilder::open(const SerialInfo& info) const
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
        //TODO: this is not atomic, need protect for concurrency
        m_impl->m_data_callback = std::move(callback);
        m_impl->start_read_loop();
    }
}

tl::expected<void, std::error_code> Serial::write(const std::span<const uint8_t> data)
{
    if (!m_impl->m_port.is_open()) return tl::make_unexpected(SerialError::DeviceDisconnected);

    const std::pmr::polymorphic_allocator<uint8_t> alloc(&m_impl->m_pool_resource);
    std::pmr::vector temp_buf(data.begin(), data.end(), alloc);

    std::binary_semaphore sem{0};
    tl::expected<void, std::error_code> res{};

    const bool was_empty = m_impl->m_write_queue.peek() == nullptr;
    m_impl->m_write_queue.enqueue(Impl::AsyncWriteReq{
        std::move(temp_buf), {}, &sem, &res
    });

    if (was_empty)
    {
        // post when spsc queue become empty
        asio::post(m_impl->m_ctx, [this]
        {
            m_impl->start_write_loop();
        });
    }

    // stuck here
    sem.acquire();

    return res;
}

void Serial::async_write(const std::span<const uint8_t> data, WriteCallback callback)
{
    if (!m_impl->m_port.is_open())
    {
        if (callback)
        {
            callback(SerialError::DeviceDisconnected, 0);
        }
        return;
    }
    m_impl->enqueue_write(data, std::move(callback));
}

void Serial::close()
{
    m_impl->close();
}

void Serial::on_disconnect(std::function<void(const std::error_code&)> callback)
{
    m_impl->m_disconnect_callback = std::move(callback);
}

void Serial::on_reconnect(std::function<void()> callback)
{
    m_impl->m_reconnect_callback = std::move(callback);
}

Serial::Serial() : m_impl(std::make_unique<Impl>())
{
}
