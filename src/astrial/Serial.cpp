#include <astrial/Serial.hpp>
#include <astrial/SerialBuilder.hpp>
#include <astrial/detail.hpp>
#include "serial_control.hpp"

#include <asio.hpp>
#include <memory_resource>
#include <readerwriterqueue.h>
#include <queue>
#include <thread>
#include <future>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

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
    FlowControl m_flow_control{FlowControl::None};
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> m_work_guard;

    asio::steady_timer m_reconnect_timer;
    asio::steady_timer m_pinout_timer;
    asio::steady_timer m_break_timer;
    std::mutex m_control_mutex; // command admission vs close/work-guard release
    std::optional<bool> m_dtr;
    std::optional<bool> m_rts;
    bool m_break_enabled{false};
    bool m_break_pulse_active{false};
    ControlCallback m_break_callback;
    uint64_t m_break_generation{};
    PinoutCallback m_pinout_callback;
    std::optional<PinoutSignals> m_pinout_baseline;
    std::chrono::milliseconds m_pinout_interval{20};
    uint64_t m_pinout_generation{};

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

    Impl() : m_ctx(1), m_port(m_ctx), m_reconnect_timer(m_ctx),
             m_pinout_timer(m_ctx), m_break_timer(m_ctx)
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
        auto break_callback = stop_controls();

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
        if (break_callback) break_callback(asio::error::operation_aborted);
    }

    void close()
    {
        const bool on_context = m_ctx.get_executor().running_in_this_thread();
        bool close_inline = false;
        {
            std::lock_guard lock(m_control_mutex);
            const bool first_close = !m_is_closed_by_user.exchange(true, std::memory_order_acq_rel);
            if (first_close)
            {
                if (on_context || !m_thread.joinable()) close_inline = true;
                else asio::post(m_ctx, [this] { close_on_context(); });
            }
            m_work_guard.reset();
        }
        if (close_inline) close_on_context();
        if (!on_context && m_thread.joinable())
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

        // Cancel control work before closing; callbacks see the disconnected
        // state, and no BREAK or sampled baseline leaks into a new connection.
        auto break_callback = stop_controls();
        // shutdown port first
        asio::error_code close_ec;
        m_port.close(close_ec);
        m_read_started = false;
        m_state.store(SerialState::Disconnected, std::memory_order_release);

        if (break_callback) break_callback(ec);
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
            auto result = detail::try_configure_port(m_port, m_port_name, m_baud_rate,
                                                     m_parity, m_stop_bits, m_data_bits,
                                                     m_flow_control);
            if (result) result = restore_modem_outputs();
            if (!result)
            {
                asio::error_code ignored;
                m_port.close(ignored);
                schedule_reconnect();
                return;
            }
            m_state.store(SerialState::Connected, std::memory_order_release);
            start_read_loop();
            start_write_loop();
            restart_pinout_monitor();
            if (!m_is_closed_by_user && m_reconnect_callback) m_reconnect_callback();
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

    // Admit commands before the work guard can be released. All accepted
    // commands complete even when another thread starts close concurrently.
    template <typename Task>
    auto run_on_context(Task task) -> std::invoke_result_t<Task>
    {
        using Result = std::invoke_result_t<Task>;
        if (m_ctx.get_executor().running_in_this_thread())
        {
            if (m_is_closed_by_user)
                return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
            return task();
        }
        auto completion = std::make_shared<std::promise<Result>>();
        auto future = completion->get_future();
        {
            std::lock_guard lock(m_control_mutex);
            if (m_is_closed_by_user || !m_thread.joinable())
                return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
            asio::post(m_ctx, [task = std::move(task), completion]() mutable
            {
                completion->set_value(task());
            });
        }
        return future.get();
    }

    tl::expected<void, std::error_code> restore_modem_outputs()
    {
        if (m_dtr)
        {
            auto result = detail::set_modem_output(m_port, true, *m_dtr);
            if (!result) return result;
        }
        if (m_rts && m_flow_control != FlowControl::Hardware)
            return detail::set_modem_output(m_port, false, *m_rts);
        return {};
    }

    tl::expected<void, std::error_code> apply_modem_output(bool dtr, bool asserted)
    {
        if (!m_port.is_open())
            return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        if (!dtr && m_flow_control == FlowControl::Hardware)
            return tl::make_unexpected(make_error_code(SerialError::UnsupportedOperation));
        auto result = detail::set_modem_output(m_port, dtr, asserted);
        if (result) (dtr ? m_dtr : m_rts) = asserted;
        return result;
    }

    tl::expected<PinoutSignals, std::error_code> read_pinout()
    {
        if (!m_port.is_open())
            return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        return detail::read_pinout(m_port);
    }

    tl::expected<void, std::error_code> apply_break(bool enabled)
    {
        if (!m_port.is_open())
            return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        if (m_break_pulse_active)
            return tl::make_unexpected(std::make_error_code(std::errc::operation_in_progress));
        auto result = detail::set_break(m_port, enabled);
        if (result) m_break_enabled = enabled;
        return result;
    }

    void start_break_pulse(std::chrono::milliseconds duration, ControlCallback callback)
    {
        auto reject = [&](const std::error_code& error) { if (callback) callback(error); };
        if (m_is_closed_by_user || !m_port.is_open())
        {
            reject(make_error_code(SerialError::DeviceDisconnected));
            return;
        }
        if (duration.count() <= 0)
        {
            reject(make_error_code(SerialError::InvalidArgument));
            return;
        }
        if (m_break_enabled || m_break_pulse_active)
        {
            reject(std::make_error_code(std::errc::operation_in_progress));
            return;
        }
        auto result = detail::set_break(m_port, true);
        if (!result) { reject(result.error()); return; }
        m_break_enabled = true;
        m_break_pulse_active = true;
        m_break_callback = std::move(callback);
        const auto generation = ++m_break_generation;
        m_break_timer.expires_after(duration);
        m_break_timer.async_wait([this, generation](const asio::error_code& error)
        {
            if (error || generation != m_break_generation) return;
            auto result = detail::set_break(m_port, false);
            if (result) m_break_enabled = false;
            m_break_pulse_active = false;
            auto callback = std::exchange(m_break_callback, {});
            if (callback) callback(result ? std::error_code{} : result.error());
        });
    }

    ControlCallback stop_controls()
    {
        ++m_pinout_generation;
        m_pinout_timer.cancel();
        m_pinout_baseline.reset();
        ++m_break_generation;
        m_break_timer.cancel();
        if (m_break_enabled && m_port.is_open()) (void)detail::set_break(m_port, false);
        m_break_enabled = false;
        m_break_pulse_active = false;
        return std::exchange(m_break_callback, {});
    }

    void schedule_pinout_poll()
    {
        if (!m_pinout_callback || !m_port.is_open() || m_is_closed_by_user) return;
        const auto generation = m_pinout_generation;
        m_pinout_timer.expires_after(m_pinout_interval);
        m_pinout_timer.async_wait([this, generation](const asio::error_code& error)
        {
            if (error || generation != m_pinout_generation || m_is_closed_by_user) return;
            auto result = read_pinout();
            auto callback = m_pinout_callback; // callback may unregister itself
            if (!result)
            {
                m_pinout_baseline.reset();
                if (callback) callback(result.error(), PinoutSignals::None);
                return;
            }
            const bool changed = m_pinout_baseline && *m_pinout_baseline != *result;
            m_pinout_baseline = *result;
            if (changed && callback) callback({}, *result);
            if (generation == m_pinout_generation) schedule_pinout_poll();
        });
    }

    void restart_pinout_monitor()
    {
        if (!m_pinout_callback) return;
        auto result = read_pinout();
        if (!result)
        {
            auto callback = m_pinout_callback;
            callback(result.error(), PinoutSignals::None);
            return;
        }
        m_pinout_baseline = *result;
        schedule_pinout_poll();
    }

    tl::expected<void, std::error_code> configure_pinout_monitor(
        PinoutCallback callback, std::chrono::milliseconds interval)
    {
        if (!m_port.is_open())
            return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        std::optional<PinoutSignals> baseline;
        if (callback)
        {
            if (interval.count() <= 0)
                return tl::make_unexpected(make_error_code(SerialError::InvalidArgument));
            auto result = read_pinout();
            if (!result) return tl::make_unexpected(result.error());
            baseline = *result;
        }
        ++m_pinout_generation;
        m_pinout_timer.cancel();
        m_pinout_callback = std::move(callback);
        m_pinout_baseline = baseline;
        m_pinout_interval = interval;
        schedule_pinout_poll();
        return {};
    }

    tl::expected<void, std::error_code> apply_baud_rate(uint32_t rate)
    {
        if (!m_port.is_open()) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        auto result = detail::set_baud_rate(m_port, rate);
        if (result) m_baud_rate = rate;
        return result;
    }

    tl::expected<void, std::error_code> apply_parity(Parity parity)
    {
        if (!m_port.is_open()) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        auto result = detail::set_parity(m_port, parity);
        if (result) m_parity = parity;
        return result;
    }

    tl::expected<void, std::error_code> apply_stop_bits(StopBits stop_bits)
    {
        if (!m_port.is_open()) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        auto result = detail::set_stop_bits(m_port, stop_bits);
        if (result) m_stop_bits = stop_bits;
        return result;
    }

    tl::expected<void, std::error_code> apply_data_bits(DataBits data_bits)
    {
        if (!m_port.is_open()) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        auto result = detail::set_data_bits(m_port, data_bits);
        if (result) m_data_bits = data_bits;
        return result;
    }

    tl::expected<void, std::error_code> apply_flow_control(FlowControl flow_control)
    {
        if (!m_port.is_open()) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        auto result = detail::set_flow_control(m_port, flow_control, m_rts);
        if (result)
        {
            m_flow_control = flow_control;
            if (flow_control == FlowControl::Hardware) m_rts.reset();
        }
        return result;
    }
};

tl::expected<Serial, std::error_code> SerialBuilder::open(const std::string_view name) const
{
    if (m_rts && m_flow_control == FlowControl::Hardware)
        return tl::make_unexpected(make_error_code(SerialError::UnsupportedOperation));
    Serial serial;
    auto& impl = *serial.m_impl;

    auto result = detail::try_configure_port(impl.m_port, name, m_rate, m_parity, m_stop_bits,
                                             m_data_bits, m_flow_control);
    if (!result) return tl::make_unexpected(result.error());

    // other param
    impl.m_port_name = name;
    impl.m_baud_rate = m_rate;
    impl.m_parity = m_parity;
    impl.m_stop_bits = m_stop_bits;
    impl.m_data_bits = m_data_bits;
    impl.m_flow_control = m_flow_control;
    impl.m_dtr = m_dtr;
    impl.m_rts = m_rts;
    result = impl.restore_modem_outputs();
    if (!result) return tl::make_unexpected(result.error());
    impl.m_auto_reconnect = m_auto_reconnect;
    impl.m_reconnect_interval = m_reconnect_interval;

    // background thread setup
    impl.m_work_guard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
        impl.m_ctx.get_executor()
    );
    impl.m_state.store(SerialState::Connected, std::memory_order_release);
    impl.m_thread = std::jthread([&impl]
    {
#ifdef _WIN32
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        impl.m_ctx.run();
    });

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

bool Serial::is_open() const noexcept
{
    return state() == SerialState::Connected;
}

SerialState Serial::state() const noexcept
{
    return m_impl ? m_impl->m_state.load(std::memory_order_acquire)
                  : SerialState::Disconnected;
}

tl::expected<void, std::error_code> Serial::set_baud_rate(const uint32_t rate)
{
    return m_impl->run_on_context([this, rate] { return m_impl->apply_baud_rate(rate); });
}

tl::expected<void, std::error_code> Serial::set_parity(const Parity parity)
{
    return m_impl->run_on_context([this, parity] { return m_impl->apply_parity(parity); });
}

tl::expected<void, std::error_code> Serial::set_stop_bits(const StopBits stop_bits)
{
    return m_impl->run_on_context([this, stop_bits] { return m_impl->apply_stop_bits(stop_bits); });
}

tl::expected<void, std::error_code> Serial::set_data_bits(const DataBits data_bits)
{
    return m_impl->run_on_context([this, data_bits] { return m_impl->apply_data_bits(data_bits); });
}

tl::expected<void, std::error_code> Serial::set_flow_control(const FlowControl flow_control)
{
    return m_impl->run_on_context([this, flow_control]
    {
        return m_impl->apply_flow_control(flow_control);
    });
}

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

tl::expected<void, std::error_code> Serial::set_data_terminal_ready(bool asserted)
{
    if (!m_impl) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
    auto* impl = m_impl.get();
    return impl->run_on_context([impl, asserted] { return impl->apply_modem_output(true, asserted); });
}

tl::expected<void, std::error_code> Serial::set_request_to_send(bool asserted)
{
    if (!m_impl) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
    auto* impl = m_impl.get();
    return impl->run_on_context([impl, asserted] { return impl->apply_modem_output(false, asserted); });
}

tl::expected<PinoutSignals, std::error_code> Serial::pinout_signals()
{
    if (!m_impl) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
    auto* impl = m_impl.get();
    return impl->run_on_context([impl] { return impl->read_pinout(); });
}

tl::expected<bool, std::error_code> Serial::is_data_terminal_ready()
{
    return pinout_signals().map([](PinoutSignals signals)
    {
        return has_signal(signals, PinoutSignals::DataTerminalReady);
    });
}

tl::expected<bool, std::error_code> Serial::is_request_to_send()
{
    return pinout_signals().map([](PinoutSignals signals)
    {
        return has_signal(signals, PinoutSignals::RequestToSend);
    });
}

tl::expected<void, std::error_code> Serial::set_break_enabled(bool enabled)
{
    if (!m_impl) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
    auto* impl = m_impl.get();
    return impl->run_on_context([impl, enabled] { return impl->apply_break(enabled); });
}

tl::expected<bool, std::error_code> Serial::is_break_enabled()
{
    if (!m_impl) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
    auto* impl = m_impl.get();
    return impl->run_on_context([impl]() -> tl::expected<bool, std::error_code>
    {
        if (!impl->m_port.is_open())
            return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        return impl->m_break_enabled;
    });
}

void Serial::async_send_break(std::chrono::milliseconds duration, ControlCallback callback)
{
    auto* impl = m_impl.get();
    if (impl)
    {
        std::lock_guard lock(impl->m_control_mutex);
        if (!impl->m_is_closed_by_user && impl->m_thread.joinable())
        {
            asio::post(impl->m_ctx, [impl, duration, callback = std::move(callback)]() mutable
            {
                impl->start_break_pulse(duration, std::move(callback));
            });
            return;
        }
    }
    if (callback) callback(make_error_code(SerialError::DeviceDisconnected));
}

tl::expected<void, std::error_code> Serial::send_break(std::chrono::milliseconds duration)
{
    if (!m_impl || m_impl->m_is_closed_by_user)
        return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
    if (m_impl->m_ctx.get_executor().running_in_this_thread())
        return tl::make_unexpected(std::make_error_code(std::errc::resource_deadlock_would_occur));
    auto completion = std::make_shared<std::promise<std::error_code>>();
    auto future = completion->get_future();
    async_send_break(duration, [completion](const std::error_code& error)
    {
        completion->set_value(error);
    });
    const auto error = future.get();
    if (error) return tl::make_unexpected(error);
    return {};
}

tl::expected<void, std::error_code> Serial::on_pinout_changed(
    PinoutCallback callback, std::chrono::milliseconds interval)
{
    if (!m_impl) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
    auto* impl = m_impl.get();
    return impl->run_on_context([impl, callback = std::move(callback), interval]() mutable
    {
        return impl->configure_pinout_monitor(std::move(callback), interval);
    });
}

tl::expected<SerialLineErrorCounters, std::error_code> Serial::line_error_counters()
{
    if (!m_impl) return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
    auto* impl = m_impl.get();
    return impl->run_on_context([impl]() -> tl::expected<SerialLineErrorCounters, std::error_code>
    {
        if (!impl->m_port.is_open())
            return tl::make_unexpected(make_error_code(SerialError::DeviceDisconnected));
        return detail::read_line_errors(impl->m_port);
    });
}
