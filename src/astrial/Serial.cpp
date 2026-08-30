#include <astrial/Serial.hpp>
#include <astrial/SerialBuilder.hpp>
#include <astrial/detail.hpp>

#include <asio.hpp>
#include <memory_resource>
#include <readerwriterqueue.h>
#include <queue>
#include <thread>
#include <future>

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

    struct SyncWriteState
    {
        std::promise<tl::expected<void, std::error_code>> promise;
    };

    struct AsyncWriteReq
    {
        std::pmr::vector<uint8_t> buffer;
        const uint8_t* borrowed_data{};
        std::size_t borrowed_size{};
        WriteCallback callback;
        std::shared_ptr<SyncWriteState> sync_state;

        [[nodiscard]] const uint8_t* data() const
        {
            return borrowed_data != nullptr ? borrowed_data : buffer.data();
        }

        [[nodiscard]] std::size_t size() const
        {
            return borrowed_data != nullptr ? borrowed_size : buffer.size();
        }
    };

    moodycamel::ReaderWriterQueue<AsyncWriteReq, ASTRIAL_WRITE_BUFFER_LENGTH> m_write_queue;
    std::atomic<size_t> m_write_count{0};
    std::pmr::synchronized_pool_resource m_pool_resource;

    std::function<void(std::span<const uint8_t>)> m_data_callback;
    ReadBufferProvider m_read_buffer_provider;
    ReadCallback m_read_callback;
    std::function<void(const asio::error_code& code)> m_disconnect_callback;
    std::function<void()> m_reconnect_callback;

    enum class ReadMode { None, Buffered, Borrowed };
    ReadMode m_read_mode{ReadMode::None};
    bool m_read_started{false};
    bool m_read_paused{false};
    bool m_write_in_flight{false};
    bool m_auto_reconnect{true};
    std::chrono::milliseconds m_reconnect_interval{std::chrono::seconds(2)};
    std::atomic<SerialState> m_state{SerialState::Disconnected};
    std::atomic<bool> m_is_closed_by_user{false};

    Impl() : m_ctx(1), m_port(m_ctx), m_reconnect_timer(m_ctx)
    {
    };

    ~Impl()
    {
        close();
    }

    void drain_all_writes(const std::error_code& ec)
    {
        while (auto* req = m_write_queue.peek())
        {
            if (req->callback) req->callback(ec, 0);
            if (req->sync_state)
            {
                req->sync_state->promise.set_value(tl::make_unexpected(ec));
            }
            m_write_queue.pop();
            m_write_count.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    void close_on_context()
    {
        m_reconnect_timer.cancel();

        if (m_port.is_open())
        {
            asio::error_code ec;
            m_port.cancel(ec);
            m_port.close(ec);
        }

        if (!m_write_in_flight)
        {
            drain_all_writes(asio::error::operation_aborted);
        }
    }

    void close()
    {
        const bool first_close = !m_is_closed_by_user.exchange(true, std::memory_order_acq_rel);

        if (first_close)
        {
            if (m_thread.joinable() && m_thread.get_id() == std::this_thread::get_id())
            {
                close_on_context();
            }
            else if (m_thread.joinable())
            {
                asio::post(m_ctx, [this] { close_on_context(); });
            }
            else
            {
                close_on_context();
            }
        }

        m_work_guard.reset();
        if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
        {
            m_thread.join();
        }

        m_state.store(SerialState::Disconnected, std::memory_order_release);
    }

    void handle_disconnection(const std::error_code& ec)
    {
        // filter error
        if (ec == asio::error::operation_aborted)
        {
            // Port was intentionally closed (e.g. Serial::close()): do not
            // notify or attempt reconnect.
            return;
        }

        // Ignore re-entrant notifications: a physical unplug can surface on
        // both the read and an in-flight write completion, and we only want a
        // single on_disconnect / reconnect sequence.
        if (m_state.load(std::memory_order_acquire) != SerialState::Connected)
        {
            return;
        }

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
                      start_write_loop();
                      // notifying user
                      if (m_reconnect_callback) m_reconnect_callback();
                      return {};
                  });
        });
    }

    void start_read_loop()
    {
        if (m_read_started || !m_port.is_open()) return;

        if (m_read_mode == ReadMode::Borrowed)
        {
            if (!m_read_buffer_provider || !m_read_callback) return;

            auto buffer = m_read_buffer_provider();
            if (buffer.empty())
            {
                m_read_paused = true;
                return;
            }

            m_read_paused = false;
            m_read_started = true;
            auto completion = m_read_callback;
            m_port.async_read_some(asio::buffer(buffer.data(), buffer.size()),
                                   [this, completion = std::move(completion)](
                                       const asio::error_code& ec, std::size_t bytes)
                                   {
                                       m_read_started = false;
                                       completion(ec, bytes);

                                       if (!ec || ec == asio::error::interrupted)
                                       {
                                           start_read_loop();
                                           return;
                                       }

                                       if (ec == asio::error::operation_aborted &&
                                           m_is_closed_by_user.load(std::memory_order_acquire))
                                       {
                                           return;
                                       }
                                       handle_disconnection(ec);
                                   });
            return;
        }

        if (m_read_mode != ReadMode::Buffered || !m_data_callback) return;
        m_read_started = true;

        struct Reader
        {
            Impl& impl;

            void operator()(const asio::error_code& ec, std::size_t bytes)
            {
                impl.m_read_started = false;
                if (!ec)
                {
                    if (bytes > 0 && impl.m_data_callback)
                    {
                        impl.m_data_callback(std::span<const uint8_t>(impl.m_rx_buffer.data(), bytes));
                    }
                    impl.start_read_loop();
                }
                else if (ec == asio::error::interrupted)
                {
                    impl.start_read_loop();
                }
                else
                {
                    if (ec == asio::error::operation_aborted && impl.m_is_closed_by_user.
                                                                     load(std::memory_order_acquire))
                    {
                        return;
                    }
                    impl.handle_disconnection(ec);
                }
            }
        };
        m_port.async_read_some(asio::buffer(m_rx_buffer), Reader{*this});
    }

    void enqueue_write(const std::span<const uint8_t> data, WriteCallback callback)
    {
        const std::pmr::polymorphic_allocator<uint8_t> alloc(&m_pool_resource);
        std::pmr::vector<uint8_t> temp_buf(data.begin(), data.end(), alloc);

        m_write_queue.enqueue(AsyncWriteReq{std::move(temp_buf), nullptr, 0,
                                            std::move(callback), {}});

        if (m_write_count.fetch_add(1, std::memory_order_acq_rel) == 0)
        {
            asio::post(m_ctx, [this]
            {
                start_write_loop();
            });
        }
    }

    void enqueue_borrowed_write(const std::span<const uint8_t> data, WriteCallback callback)
    {
        const std::pmr::polymorphic_allocator<uint8_t> alloc(&m_pool_resource);
        std::pmr::vector<uint8_t> empty_buf(alloc);
        m_write_queue.enqueue(AsyncWriteReq{std::move(empty_buf), data.data(), data.size(),
                                            std::move(callback), {}});

        if (m_write_count.fetch_add(1, std::memory_order_acq_rel) == 0)
        {
            asio::post(m_ctx, [this]
            {
                start_write_loop();
            });
        }
    }

    void start_write_loop()
    {
        if (m_write_in_flight) return;
        auto request = m_write_queue.peek();
        if (request == nullptr) return; // empty

        m_write_in_flight = true;
        asio::async_write(m_port, asio::buffer(request->data(), request->size()),
                          [this, request](const asio::error_code& ec, std::size_t bytes_transferred)
                          {
                              m_write_in_flight = false;
                              if (!ec)
                              {
                                  if (request->callback)
                                  {
                                      request->callback(ec, bytes_transferred);
                                  }
                                   if (request->sync_state)
                                   {
                                       request->sync_state->promise.set_value({});
                                   }
                                  m_write_queue.pop();

                                  if (m_write_count.fetch_sub(1, std::memory_order_acq_rel) > 1)
                                  {
                                      start_write_loop();
                                  }
                              }
                              else if (ec == asio::error::interrupted)
                              {
                                  start_write_loop();
                              }
                              else
                              {
                                  drain_all_writes(ec);
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
    return serial;
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
    asio::post(m_impl->m_ctx, [this, cb = std::move(callback)]() mutable
    {
        if (m_impl->m_read_mode == Impl::ReadMode::Borrowed) return;
        m_impl->m_read_mode = Impl::ReadMode::Buffered;
        m_impl->m_data_callback = std::move(cb);
        m_impl->start_read_loop();
    });
}

void Serial::on_data_borrowed(ReadBufferProvider provider, ReadCallback callback)
{
    asio::post(m_impl->m_ctx,
               [this, provider = std::move(provider), callback = std::move(callback)]() mutable
               {
                   if (m_impl->m_read_mode == Impl::ReadMode::Buffered)
                   {
                       if (callback)
                       {
                           callback(make_error_code(SerialError::InvalidArgument), 0);
                       }
                       return;
                   }
                   m_impl->m_read_mode = Impl::ReadMode::Borrowed;
                   m_impl->m_read_buffer_provider = std::move(provider);
                   m_impl->m_read_callback = std::move(callback);
                   m_impl->start_read_loop();
               });
}

void Serial::resume_read()
{
    asio::post(m_impl->m_ctx, [this]
    {
        if (m_impl->m_read_mode == Impl::ReadMode::Borrowed && m_impl->m_read_paused)
        {
            m_impl->start_read_loop();
        }
    });
}

tl::expected<void, std::error_code> Serial::write(const std::span<const uint8_t> data)
{
    if (!m_impl->m_port.is_open()) return tl::make_unexpected(SerialError::DeviceDisconnected);

    const std::pmr::polymorphic_allocator<uint8_t> alloc(&m_impl->m_pool_resource);
    std::pmr::vector<uint8_t> temp_buf(data.begin(), data.end(), alloc);

    auto state = std::make_shared<Impl::SyncWriteState>();
    auto future = state->promise.get_future();

    m_impl->m_write_queue.enqueue(Impl::AsyncWriteReq{
        std::move(temp_buf), nullptr, 0, {}, state
    });

    if (m_impl->m_write_count.fetch_add(1, std::memory_order_acq_rel) == 0)
    {
        asio::post(m_impl->m_ctx, [this]
        {
            m_impl->start_write_loop();
        });
    }

    return future.get();
}

tl::expected<void, std::error_code> Serial::write(const std::span<const uint8_t> data, std::chrono::milliseconds timeout)
{
    if (!m_impl->m_port.is_open()) return tl::make_unexpected(SerialError::DeviceDisconnected);

    const std::pmr::polymorphic_allocator<uint8_t> alloc(&m_impl->m_pool_resource);
    std::pmr::vector<uint8_t> temp_buf(data.begin(), data.end(), alloc);

    auto state = std::make_shared<Impl::SyncWriteState>();
    auto future = state->promise.get_future();

    m_impl->m_write_queue.enqueue(Impl::AsyncWriteReq{
        std::move(temp_buf), nullptr, 0, {}, state
    });

    if (m_impl->m_write_count.fetch_add(1, std::memory_order_acq_rel) == 0)
    {
        asio::post(m_impl->m_ctx, [this]
        {
            m_impl->start_write_loop();
        });
    }

    if (future.wait_for(timeout) == std::future_status::ready)
    {
        return future.get();
    }
    return tl::make_unexpected(make_error_code(SerialError::WriteTimeout));
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

void Serial::async_write_borrowed(const std::span<const uint8_t> data, WriteCallback callback)
{
    if (!m_impl->m_port.is_open())
    {
        if (callback)
        {
            callback(SerialError::DeviceDisconnected, 0);
        }
        return;
    }
    m_impl->enqueue_borrowed_write(data, std::move(callback));
}

void Serial::close()
{
    m_impl->close();
}

void Serial::on_disconnect(std::function<void(const std::error_code&)> callback)
{
    asio::post(m_impl->m_ctx, [this, cb = std::move(callback)]() mutable
    {
        m_impl->m_disconnect_callback = std::move(cb);
    });
}

void Serial::on_reconnect(std::function<void()> callback)
{
    asio::post(m_impl->m_ctx, [this, cb = std::move(callback)]() mutable
    {
        m_impl->m_reconnect_callback = std::move(cb);
    });
}

Serial::Serial() : m_impl(std::make_unique<Impl>())
{
}
