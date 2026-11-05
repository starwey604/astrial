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

    // True while the port is open and connected. During automatic reconnect the
    // transport is closed, so is_open() is false and state() is Reconnecting.
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] SerialState state() const noexcept;

    // Runtime reconfiguration. Applied on the I/O thread while the port is
    // open, and stored so an automatic reconnect reopens with the new settings.
    // Returns DeviceDisconnected when the port is not open.
    tl::expected<void, std::error_code> set_baud_rate(uint32_t rate);
    tl::expected<void, std::error_code> set_parity(Parity parity);
    tl::expected<void, std::error_code> set_stop_bits(StopBits stop_bits);
    tl::expected<void, std::error_code> set_data_bits(DataBits data_bits);
    tl::expected<void, std::error_code> set_flow_control(FlowControl flow_control);

    // RS-232 controls. All execute on the I/O thread and require an open port.
    // true means asserted (positive RS-232 control voltage), not TTL-high.
    // Successful explicit DTR/RTS settings are restored on reconnect. Enabling
    // hardware flow control relinquishes the saved manual RTS setting.
    tl::expected<void, std::error_code> set_data_terminal_ready(bool asserted);
    tl::expected<void, std::error_code> set_request_to_send(bool asserted);
    tl::expected<bool, std::error_code> is_data_terminal_ready();
    tl::expected<bool, std::error_code> is_request_to_send();
    tl::expected<PinoutSignals, std::error_code> pinout_signals();

    // Persistent BREAK, cleared on close/disconnect and never restored.
    // is_break_enabled() reports Astrial's successfully applied local state.
    tl::expected<void, std::error_code> set_break_enabled(bool enabled);
    tl::expected<bool, std::error_code> is_break_enabled();

    using ControlCallback = std::function<void(const std::error_code&)>;
    // Sends a timed BREAK without blocking the I/O thread. Does not drain TX.
    // While active, another BREAK request fails with operation_in_progress.
    // Duration is a host timer minimum, not a hardware timing guarantee.
    // Accepted callbacks run exactly once on the I/O thread, including cancel.
    // An immediately rejected request invokes its callback on the caller.
    void async_send_break(std::chrono::milliseconds duration, ControlCallback callback);
    // Blocks the caller, not the I/O thread. From an I/O callback this returns
    // resource_deadlock_would_occur; use async_send_break() there instead.
    tl::expected<void, std::error_code> send_break(
        std::chrono::milliseconds duration = std::chrono::milliseconds(250));

    using PinoutCallback = std::function<void(const std::error_code&, PinoutSignals)>;
    // Opt-in sampled changes, not edge capture: pulses shorter than interval
    // may be missed. Registration reads a baseline (no initial callback).
    // Callbacks execute on the I/O thread. An empty callback disables polling.
    // A query error is delivered once and stops polling until reconnect or
    // re-registration. Monitoring resumes with a new baseline on reconnect.
    tl::expected<void, std::error_code> on_pinout_changed(
        PinoutCallback callback,
        std::chrono::milliseconds interval = std::chrono::milliseconds(20));

    // No synthesized counters: returns UnsupportedOperation if unavailable.
    tl::expected<SerialLineErrorCounters, std::error_code> line_error_counters();

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
