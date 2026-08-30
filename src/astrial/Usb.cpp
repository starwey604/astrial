/* SPDX-License-Identifier: MIT */

#include <astrial/Usb.hpp>

#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
class UsbErrorCategory final : public std::error_category
{
public:
    const char* name() const noexcept override
    {
        return "astrial.usb";
    }

    std::string message(int value) const override
    {
        switch (static_cast<UsbError>(value))
        {
        case UsbError::Success: return "success";
        case UsbError::DeviceNotFound: return "USB device not found";
        case UsbError::PermissionDenied: return "permission denied";
        case UsbError::InterfaceBusy: return "USB interface busy";
        case UsbError::InvalidArgument: return "invalid argument";
        case UsbError::DeviceDisconnected: return "USB device disconnected";
        case UsbError::TransferTimeout: return "USB transfer timed out";
        case UsbError::TransferCancelled: return "USB transfer cancelled";
        case UsbError::TransferStalled: return "USB endpoint stalled";
        case UsbError::TransferOverflow: return "USB transfer overflow";
        case UsbError::NotSupported: return "operation not supported";
        case UsbError::IoError: return "USB I/O error";
        }
        return "unknown USB error";
    }
};

std::error_code from_libusb_error(int value)
{
    switch (value)
    {
    case LIBUSB_SUCCESS: return {};
    case LIBUSB_ERROR_ACCESS: return UsbError::PermissionDenied;
    case LIBUSB_ERROR_BUSY: return UsbError::InterfaceBusy;
    case LIBUSB_ERROR_INVALID_PARAM: return UsbError::InvalidArgument;
    case LIBUSB_ERROR_NO_DEVICE: return UsbError::DeviceDisconnected;
    case LIBUSB_ERROR_NOT_FOUND: return UsbError::DeviceNotFound;
    case LIBUSB_ERROR_TIMEOUT: return UsbError::TransferTimeout;
    case LIBUSB_ERROR_INTERRUPTED: return UsbError::TransferCancelled;
    case LIBUSB_ERROR_PIPE: return UsbError::TransferStalled;
    case LIBUSB_ERROR_OVERFLOW: return UsbError::TransferOverflow;
    case LIBUSB_ERROR_NOT_SUPPORTED: return UsbError::NotSupported;
    default: return UsbError::IoError;
    }
}

std::error_code from_transfer_status(libusb_transfer_status status)
{
    switch (status)
    {
    case LIBUSB_TRANSFER_COMPLETED: return {};
    case LIBUSB_TRANSFER_ERROR: return UsbError::IoError;
    case LIBUSB_TRANSFER_TIMED_OUT: return UsbError::TransferTimeout;
    case LIBUSB_TRANSFER_CANCELLED: return UsbError::TransferCancelled;
    case LIBUSB_TRANSFER_STALL: return UsbError::TransferStalled;
    case LIBUSB_TRANSFER_NO_DEVICE: return UsbError::DeviceDisconnected;
    case LIBUSB_TRANSFER_OVERFLOW: return UsbError::TransferOverflow;
    }
    return UsbError::IoError;
}

std::string read_string(libusb_device_handle* handle, uint8_t index)
{
    if (handle == nullptr || index == 0) return {};

    unsigned char value[256]{};
    const int size = libusb_get_string_descriptor_ascii(handle, index, value,
                                                         sizeof(value));
    if (size <= 0) return {};
    return {reinterpret_cast<const char*>(value), static_cast<std::size_t>(size)};
}

bool valid_config(const UsbBulkConfig& config)
{
    return config.device.vendor_id != 0 && config.device.product_id != 0 &&
           (config.bulk_interface.endpoint_in & LIBUSB_ENDPOINT_DIR_MASK) ==
               LIBUSB_ENDPOINT_IN &&
           (config.bulk_interface.endpoint_out & LIBUSB_ENDPOINT_DIR_MASK) ==
               LIBUSB_ENDPOINT_OUT &&
           config.read_queue_depth > 0 && config.read_queue_depth <= 32 &&
           config.reconnect_interval.count() >= 0;
}

bool matches_port_path(libusb_device* device, const std::vector<uint8_t>& expected)
{
    if (expected.empty()) return true;
    uint8_t actual[8]{};
    const int count = libusb_get_port_numbers(device, actual,
                                               static_cast<int>(std::size(actual)));
    return count >= 0 && expected.size() == static_cast<std::size_t>(count) &&
           std::equal(expected.begin(), expected.end(), actual);
}

tl::expected<UsbBulkEndpointInfo, std::error_code>
inspect_bulk_interface(libusb_device* device, const UsbBulkInterface& wanted)
{
    libusb_config_descriptor* config{};
    const int result = libusb_get_active_config_descriptor(device, &config);
    if (result != LIBUSB_SUCCESS)
    {
        return tl::make_unexpected(from_libusb_error(result));
    }

    UsbBulkEndpointInfo info{};
    bool interface_found = false;
    for (uint8_t index = 0; index < config->bNumInterfaces; ++index)
    {
        const auto& interface = config->interface[index];
        for (int alternate = 0; alternate < interface.num_altsetting; ++alternate)
        {
            const auto& descriptor = interface.altsetting[alternate];
            if (descriptor.bInterfaceNumber != wanted.interface_number ||
                descriptor.bAlternateSetting != 0)
            {
                continue;
            }
            interface_found = true;
            for (uint8_t endpoint = 0; endpoint < descriptor.bNumEndpoints; ++endpoint)
            {
                const auto& ep = descriptor.endpoint[endpoint];
                if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                    LIBUSB_TRANSFER_TYPE_BULK)
                {
                    continue;
                }
                if (ep.bEndpointAddress == wanted.endpoint_in)
                {
                    info.maximum_packet_size_in = ep.wMaxPacketSize & 0x07ffU;
                }
                else if (ep.bEndpointAddress == wanted.endpoint_out)
                {
                    info.maximum_packet_size_out = ep.wMaxPacketSize & 0x07ffU;
                }
            }
        }
    }
    libusb_free_config_descriptor(config);

    if (!interface_found || info.maximum_packet_size_in == 0 ||
        info.maximum_packet_size_out == 0)
    {
        return tl::make_unexpected(make_error_code(UsbError::InvalidArgument));
    }
    return info;
}
} // namespace

const std::error_category& get_usb_category()
{
    static UsbErrorCategory category;
    return category;
}

std::error_code make_error_code(UsbError error)
{
    return {static_cast<int>(error), get_usb_category()};
}

class UsbBulkDevice::Impl
{
public:
    struct ReadSlot
    {
        Impl* owner{};
        libusb_transfer* transfer{};
        UsbBorrowedBuffer buffer{};
        std::atomic<bool> submitted{false};
    };

    explicit Impl(UsbBulkConfig config) : m_config(std::move(config))
    {
    }

    ~Impl()
    {
        close();
    }

    tl::expected<void, std::error_code> initialize()
    {
        const int init_result = libusb_init(&m_context);
        if (init_result != LIBUSB_SUCCESS)
        {
            return tl::make_unexpected(from_libusb_error(init_result));
        }

        auto open_result = open_matching_device();
        if (!open_result) return open_result;

        m_read_slots.reserve(m_config.read_queue_depth);
        for (std::size_t index = 0; index < m_config.read_queue_depth; ++index)
        {
            auto slot = std::make_unique<ReadSlot>();
            slot->owner = this;
            slot->transfer = libusb_alloc_transfer(0);
            if (slot->transfer == nullptr)
            {
                return tl::make_unexpected(make_error_code(UsbError::IoError));
            }
            m_read_slots.push_back(std::move(slot));
        }

        m_write_transfer = libusb_alloc_transfer(0);
        if (m_write_transfer == nullptr)
        {
            return tl::make_unexpected(make_error_code(UsbError::IoError));
        }

        m_state.store(UsbState::Connected, std::memory_order_release);
        m_event_thread = std::thread([this] { event_loop(); });
        return {};
    }

    tl::expected<void, std::error_code> start_reads(ReadBufferProvider provider,
                                                    ReadCallback callback)
    {
        if (!provider || !callback)
        {
            return tl::make_unexpected(make_error_code(UsbError::InvalidArgument));
        }
        if (m_stopping.load(std::memory_order_acquire))
        {
            return tl::make_unexpected(make_error_code(UsbError::DeviceDisconnected));
        }

        std::lock_guard lifecycle_lock(m_read_lifecycle_mutex);
        if (m_reads_enabled.load())
        {
            return tl::make_unexpected(make_error_code(UsbError::InterfaceBusy));
        }
        m_read_provider = std::move(provider);
        m_read_callback = std::move(callback);
        m_reads_enabled.store(true);
        wake_event_thread();
        return {};
    }

    tl::expected<void, std::error_code> stop_reads()
    {
        if (m_event_thread.joinable() &&
            m_event_thread.get_id() == std::this_thread::get_id())
        {
            return tl::make_unexpected(make_error_code(UsbError::InterfaceBusy));
        }

        std::lock_guard lifecycle_lock(m_read_lifecycle_mutex);
        if (!m_reads_enabled.exchange(false)) return {};

        while (m_arming_reads.load()) m_arming_reads.wait(true);
        for (const auto& slot : m_read_slots)
        {
            if (slot->submitted.load(std::memory_order_acquire))
            {
                if (libusb_cancel_transfer(slot->transfer) == LIBUSB_SUCCESS)
                {
                    m_cancellations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        wake_event_thread();

        for (const auto& slot : m_read_slots)
        {
            while (slot->submitted.load(std::memory_order_acquire))
            {
                slot->submitted.wait(true, std::memory_order_acquire);
            }
        }
        m_read_provider = {};
        m_read_callback = {};
        return {};
    }

    void resume_reads()
    {
        wake_event_thread();
    }

    tl::expected<void, std::error_code>
    async_write_borrowed(std::span<const uint8_t> data, WriteCallback callback)
    {
        if (data.size() > static_cast<std::size_t>(INT_MAX))
        {
            return tl::make_unexpected(make_error_code(UsbError::InvalidArgument));
        }
        bool inactive = false;
        if (!m_write_active.compare_exchange_strong(inactive, true,
                                                    std::memory_order_acq_rel))
        {
            return tl::make_unexpected(make_error_code(UsbError::InterfaceBusy));
        }
        if (m_stopping.load(std::memory_order_acquire) ||
            m_state.load(std::memory_order_acquire) != UsbState::Connected)
        {
            m_write_active.store(false, std::memory_order_release);
            return tl::make_unexpected(make_error_code(UsbError::DeviceDisconnected));
        }

        m_write_callback = std::move(callback);
        m_write_size = data.size();
        m_write_needs_zlp = !data.empty() &&
                            m_endpoint_info.maximum_packet_size_out > 0 &&
                            data.size() % m_endpoint_info.maximum_packet_size_out == 0;
        m_write_zlp_phase = false;
        libusb_fill_bulk_transfer(m_write_transfer, m_handle,
                                  m_config.bulk_interface.endpoint_out,
                                  const_cast<unsigned char*>(data.data()),
                                  static_cast<int>(data.size()), write_completed, this, 0);
        m_write_submitted.store(true, std::memory_order_release);
        const int result = libusb_submit_transfer(m_write_transfer);
        if (result != LIBUSB_SUCCESS)
        {
            m_write_submitted.store(false, std::memory_order_release);
            m_write_callback = {};
            m_write_active.store(false, std::memory_order_release);
            m_errors.fetch_add(1, std::memory_order_relaxed);
            return tl::make_unexpected(from_libusb_error(result));
        }
        m_writes_submitted.fetch_add(1, std::memory_order_relaxed);
        if (m_stopping.load(std::memory_order_acquire) &&
            libusb_cancel_transfer(m_write_transfer) == LIBUSB_SUCCESS)
        {
            m_cancellations.fetch_add(1, std::memory_order_relaxed);
        }
        return {};
    }

    void set_disconnect_callback(std::function<void(const std::error_code&)> callback)
    {
        std::lock_guard lock(m_mutex);
        m_disconnect_callback = std::move(callback);
    }

    void set_reconnect_callback(std::function<void()> callback)
    {
        std::lock_guard lock(m_mutex);
        m_reconnect_callback = std::move(callback);
    }

    [[nodiscard]] UsbState state() const noexcept
    {
        return m_state.load(std::memory_order_acquire);
    }

    [[nodiscard]] UsbBulkEndpointInfo endpoint_info() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return m_endpoint_info;
    }

    [[nodiscard]] UsbBulkStats stats() const noexcept
    {
        return {
            .reads_submitted = m_reads_submitted.load(std::memory_order_relaxed),
            .reads_completed = m_reads_completed.load(std::memory_order_relaxed),
            .bytes_received = m_bytes_received.load(std::memory_order_relaxed),
            .writes_submitted = m_writes_submitted.load(std::memory_order_relaxed),
            .writes_completed = m_writes_completed.load(std::memory_order_relaxed),
            .bytes_transmitted = m_bytes_transmitted.load(std::memory_order_relaxed),
            .cancellations = m_cancellations.load(std::memory_order_relaxed),
            .errors = m_errors.load(std::memory_order_relaxed),
            .reconnects = m_reconnects.load(std::memory_order_relaxed),
        };
    }

    void close()
    {
        const bool first_close = !m_stopping.exchange(true, std::memory_order_acq_rel);
        if (first_close)
        {
            m_state.store(UsbState::Closed, std::memory_order_release);
            for (const auto& slot : m_read_slots)
            {
                if (slot->submitted.load(std::memory_order_acquire))
                {
                    if (libusb_cancel_transfer(slot->transfer) == LIBUSB_SUCCESS)
                    {
                        m_cancellations.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            if (m_write_submitted.load(std::memory_order_acquire))
            {
                if (libusb_cancel_transfer(m_write_transfer) == LIBUSB_SUCCESS)
                {
                    m_cancellations.fetch_add(1, std::memory_order_relaxed);
                }
            }
            wake_event_thread();
        }

        // close() is valid from a completion callback. Cleanup is deferred
        // until a different thread destroys the object or calls close again.
        if (m_event_thread.joinable() &&
            m_event_thread.get_id() == std::this_thread::get_id())
        {
            return;
        }
        if (m_event_thread.joinable())
        {
            m_event_thread.join();
        }

        if (m_cleanup_done.exchange(true, std::memory_order_acq_rel)) return;
        for (const auto& slot : m_read_slots)
        {
            libusb_free_transfer(slot->transfer);
        }
        m_read_slots.clear();
        if (m_write_transfer != nullptr)
        {
            libusb_free_transfer(m_write_transfer);
            m_write_transfer = nullptr;
        }
        close_handle();
        if (m_context != nullptr)
        {
            libusb_exit(m_context);
            m_context = nullptr;
        }
    }

private:
    tl::expected<void, std::error_code> open_matching_device()
    {
        libusb_device** devices{};
        const ssize_t count = libusb_get_device_list(m_context, &devices);
        if (count < 0)
        {
            return tl::make_unexpected(from_libusb_error(static_cast<int>(count)));
        }

        std::error_code last_error = UsbError::DeviceNotFound;
        for (ssize_t index = 0; index < count; ++index)
        {
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(devices[index], &descriptor) != LIBUSB_SUCCESS ||
                descriptor.idVendor != m_config.device.vendor_id ||
                descriptor.idProduct != m_config.device.product_id ||
                !matches_port_path(devices[index], m_config.device.port_path))
            {
                continue;
            }

            libusb_device_handle* candidate{};
            const int open_result = libusb_open(devices[index], &candidate);
            if (open_result != LIBUSB_SUCCESS)
            {
                last_error = from_libusb_error(open_result);
                continue;
            }

            if (!m_config.device.serial_number.empty() &&
                read_string(candidate, descriptor.iSerialNumber) !=
                    m_config.device.serial_number)
            {
                libusb_close(candidate);
                continue;
            }

            auto endpoint_info =
                inspect_bulk_interface(devices[index], m_config.bulk_interface);
            if (!endpoint_info)
            {
                last_error = endpoint_info.error();
                libusb_close(candidate);
                continue;
            }

            if (m_config.auto_detach_kernel_driver)
            {
                const int detach_result = libusb_set_auto_detach_kernel_driver(candidate, 1);
                if (detach_result != LIBUSB_SUCCESS &&
                    detach_result != LIBUSB_ERROR_NOT_SUPPORTED)
                {
                    last_error = from_libusb_error(detach_result);
                    libusb_close(candidate);
                    continue;
                }
            }

            const int claim_result = libusb_claim_interface(
                candidate, m_config.bulk_interface.interface_number);
            if (claim_result != LIBUSB_SUCCESS)
            {
                last_error = from_libusb_error(claim_result);
                libusb_close(candidate);
                continue;
            }

            m_handle = candidate;
            {
                std::lock_guard lock(m_mutex);
                m_endpoint_info = endpoint_info.value();
            }
            libusb_free_device_list(devices, 1);
            return {};
        }

        libusb_free_device_list(devices, 1);
        return tl::make_unexpected(last_error);
    }

    void close_handle()
    {
        if (m_handle == nullptr) return;
        (void)libusb_release_interface(m_handle,
                                       m_config.bulk_interface.interface_number);
        libusb_close(m_handle);
        m_handle = nullptr;
    }

    bool transfers_active() const
    {
        if (m_write_active.load(std::memory_order_acquire)) return true;
        return std::any_of(m_read_slots.begin(), m_read_slots.end(), [](const auto& slot)
        {
            return slot->submitted.load(std::memory_order_acquire);
        });
    }

    void wake_event_thread()
    {
        if (m_context != nullptr) libusb_interrupt_event_handler(m_context);
    }

    void event_loop()
    {
        while (!m_stopping.load(std::memory_order_acquire))
        {
            if (m_state.load(std::memory_order_acquire) == UsbState::Connected)
            {
                arm_reads();
            }
            else if (m_state.load(std::memory_order_acquire) == UsbState::Reconnecting &&
                     std::chrono::steady_clock::now() >= m_next_reconnect &&
                     !transfers_active())
            {
                attempt_reconnect();
            }

            timeval timeout{0, 10000};
            const int result = libusb_handle_events_timeout_completed(m_context, &timeout,
                                                                       nullptr);
            if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_INTERRUPTED)
            {
                mark_disconnected(from_libusb_error(result));
            }
        }

        while (transfers_active())
        {
            timeval timeout{0, 10000};
            (void)libusb_handle_events_timeout_completed(m_context, &timeout, nullptr);
        }
    }

    void arm_reads()
    {
        m_arming_reads.store(true);
        if (!m_reads_enabled.load())
        {
            m_arming_reads.store(false);
            m_arming_reads.notify_all();
            return;
        }

        for (const auto& slot : m_read_slots)
        {
            if (slot->submitted.load(std::memory_order_acquire)) continue;

            slot->buffer = m_read_provider();
            if (slot->buffer.bytes.empty()) continue;
            if (slot->buffer.bytes.size() > static_cast<std::size_t>(INT_MAX))
            {
                deliver_read(*slot, UsbError::InvalidArgument, 0);
                continue;
            }

            libusb_fill_bulk_transfer(slot->transfer, m_handle,
                                      m_config.bulk_interface.endpoint_in,
                                      slot->buffer.bytes.data(),
                                      static_cast<int>(slot->buffer.bytes.size()),
                                      read_completed, slot.get(), 0);
            slot->submitted.store(true, std::memory_order_release);
            const int result = libusb_submit_transfer(slot->transfer);
            if (result != LIBUSB_SUCCESS)
            {
                slot->submitted.store(false, std::memory_order_release);
                m_errors.fetch_add(1, std::memory_order_relaxed);
                deliver_read(*slot, from_libusb_error(result), 0);
                if (result == LIBUSB_ERROR_NO_DEVICE)
                {
                    mark_disconnected(UsbError::DeviceDisconnected);
                    break;
                }
            }
            else
            {
                m_reads_submitted.fetch_add(1, std::memory_order_relaxed);
            }
        }
        m_arming_reads.store(false);
        m_arming_reads.notify_all();
    }

    static void LIBUSB_CALL read_completed(libusb_transfer* transfer)
    {
        auto& slot = *static_cast<ReadSlot*>(transfer->user_data);
        Impl& self = *slot.owner;
        const auto error = from_transfer_status(transfer->status);
        self.deliver_read(slot, error,
                          transfer->actual_length > 0
                              ? static_cast<std::size_t>(transfer->actual_length)
                              : 0);
        self.m_reads_completed.fetch_add(1, std::memory_order_relaxed);
        if (!error)
        {
            self.m_bytes_received.fetch_add(
                transfer->actual_length > 0
                    ? static_cast<std::size_t>(transfer->actual_length)
                    : 0,
                std::memory_order_relaxed);
        }
        else if (error != make_error_code(UsbError::TransferCancelled))
        {
            self.m_errors.fetch_add(1, std::memory_order_relaxed);
        }
        slot.submitted.store(false, std::memory_order_release);
        slot.submitted.notify_all();
        if (error == make_error_code(UsbError::DeviceDisconnected))
        {
            self.mark_disconnected(error);
        }
    }

    void deliver_read(ReadSlot& slot, const std::error_code& error, std::size_t size)
    {
        auto buffer = std::exchange(slot.buffer, {});
        if (m_read_callback) m_read_callback(error, buffer, size);
    }

    static void LIBUSB_CALL write_completed(libusb_transfer* transfer)
    {
        static_cast<Impl*>(transfer->user_data)->handle_write_completion(transfer);
    }

    void handle_write_completion(libusb_transfer* transfer)
    {
        auto error = from_transfer_status(transfer->status);
        if (!error && m_write_needs_zlp && !m_write_zlp_phase)
        {
            m_write_zlp_phase = true;
            libusb_fill_bulk_transfer(m_write_transfer, m_handle,
                                      m_config.bulk_interface.endpoint_out,
                                      nullptr, 0, write_completed, this, 0);
            const int result = libusb_submit_transfer(m_write_transfer);
            if (result == LIBUSB_SUCCESS) return;
            error = from_libusb_error(result);
        }

        WriteCallback callback = std::move(m_write_callback);
        const std::size_t transferred = !error ? m_write_size : 0;
        m_write_size = 0;
        m_write_needs_zlp = false;
        m_write_zlp_phase = false;
        m_write_submitted.store(false, std::memory_order_release);
        if (error == make_error_code(UsbError::DeviceDisconnected))
        {
            mark_disconnected(error);
        }
        m_write_active.store(false, std::memory_order_release);
        if (callback) callback(error, transferred);
        m_writes_completed.fetch_add(1, std::memory_order_relaxed);
        if (!error)
        {
            m_bytes_transmitted.fetch_add(transferred, std::memory_order_relaxed);
        }
        else if (error != make_error_code(UsbError::TransferCancelled))
        {
            m_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void mark_disconnected(const std::error_code& error)
    {
        UsbState expected = UsbState::Connected;
        const UsbState target = m_config.auto_reconnect ? UsbState::Reconnecting
                                                        : UsbState::Disconnected;
        if (!m_state.compare_exchange_strong(expected, target, std::memory_order_acq_rel))
        {
            return;
        }

        for (const auto& slot : m_read_slots)
        {
            if (slot->submitted.load(std::memory_order_acquire))
            {
                if (libusb_cancel_transfer(slot->transfer) == LIBUSB_SUCCESS)
                {
                    m_cancellations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (m_write_submitted.load(std::memory_order_acquire))
        {
            if (libusb_cancel_transfer(m_write_transfer) == LIBUSB_SUCCESS)
            {
                m_cancellations.fetch_add(1, std::memory_order_relaxed);
            }
        }
        m_next_reconnect = std::chrono::steady_clock::now() + m_config.reconnect_interval;

        std::function<void(const std::error_code&)> callback;
        {
            std::lock_guard lock(m_mutex);
            callback = m_disconnect_callback;
        }
        if (callback) callback(error);
    }

    void attempt_reconnect()
    {
        close_handle();
        auto result = open_matching_device();
        if (!result)
        {
            m_next_reconnect = std::chrono::steady_clock::now() + m_config.reconnect_interval;
            return;
        }

        m_state.store(UsbState::Connected, std::memory_order_release);
        m_reconnects.fetch_add(1, std::memory_order_relaxed);
        std::function<void()> callback;
        {
            std::lock_guard lock(m_mutex);
            callback = m_reconnect_callback;
        }
        if (callback) callback();
    }

    UsbBulkConfig m_config;
    libusb_context* m_context{};
    libusb_device_handle* m_handle{};
    UsbBulkEndpointInfo m_endpoint_info{};
    std::vector<std::unique_ptr<ReadSlot>> m_read_slots;
    libusb_transfer* m_write_transfer{};
    std::thread m_event_thread;

    mutable std::mutex m_mutex;
    std::mutex m_read_lifecycle_mutex;
    ReadBufferProvider m_read_provider;
    ReadCallback m_read_callback;
    WriteCallback m_write_callback;
    std::function<void(const std::error_code&)> m_disconnect_callback;
    std::function<void()> m_reconnect_callback;
    std::atomic<bool> m_reads_enabled{false};
    std::atomic<bool> m_arming_reads{false};
    std::atomic<bool> m_write_active{false};
    std::atomic<bool> m_write_submitted{false};
    std::size_t m_write_size{};
    bool m_write_needs_zlp{};
    bool m_write_zlp_phase{};
    std::atomic<UsbState> m_state{UsbState::Disconnected};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_cleanup_done{false};
    std::atomic<uint64_t> m_reads_submitted{};
    std::atomic<uint64_t> m_reads_completed{};
    std::atomic<uint64_t> m_bytes_received{};
    std::atomic<uint64_t> m_writes_submitted{};
    std::atomic<uint64_t> m_writes_completed{};
    std::atomic<uint64_t> m_bytes_transmitted{};
    std::atomic<uint64_t> m_cancellations{};
    std::atomic<uint64_t> m_errors{};
    std::atomic<uint64_t> m_reconnects{};
    std::chrono::steady_clock::time_point m_next_reconnect{};
};

tl::expected<std::vector<UsbDeviceInfo>, std::error_code> UsbBulkDevice::list_devices()
{
    libusb_context* context{};
    const int init_result = libusb_init(&context);
    if (init_result != LIBUSB_SUCCESS)
    {
        return tl::make_unexpected(from_libusb_error(init_result));
    }

    libusb_device** devices{};
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0)
    {
        libusb_exit(context);
        return tl::make_unexpected(from_libusb_error(static_cast<int>(count)));
    }

    std::vector<UsbDeviceInfo> result;
    result.reserve(static_cast<std::size_t>(count));
    for (ssize_t index = 0; index < count; ++index)
    {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[index], &descriptor) != LIBUSB_SUCCESS)
        {
            continue;
        }

        UsbDeviceInfo info;
        info.vendor_id = descriptor.idVendor;
        info.product_id = descriptor.idProduct;
        info.bus_number = libusb_get_bus_number(devices[index]);
        info.device_address = libusb_get_device_address(devices[index]);
        uint8_t ports[8]{};
        const int port_count = libusb_get_port_numbers(devices[index], ports,
                                                       static_cast<int>(std::size(ports)));
        if (port_count > 0) info.port_path.assign(ports, ports + port_count);

        libusb_device_handle* handle{};
        if (libusb_open(devices[index], &handle) == LIBUSB_SUCCESS)
        {
            info.manufacturer = read_string(handle, descriptor.iManufacturer);
            info.product = read_string(handle, descriptor.iProduct);
            info.serial_number = read_string(handle, descriptor.iSerialNumber);
            libusb_close(handle);
        }
        result.push_back(std::move(info));
    }

    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return result;
}

tl::expected<UsbBulkDevice, std::error_code> UsbBulkDevice::open(const UsbBulkConfig& config)
{
    if (!valid_config(config))
    {
        return tl::make_unexpected(make_error_code(UsbError::InvalidArgument));
    }

    auto implementation = std::make_unique<Impl>(config);
    auto result = implementation->initialize();
    if (!result) return tl::make_unexpected(result.error());
    return UsbBulkDevice(std::move(implementation));
}

tl::expected<UsbBulkDevice, std::error_code>
UsbBulkDevice::open(const UsbDeviceInfo& device, UsbBulkConfig config)
{
    config.device.vendor_id = device.vendor_id;
    config.device.product_id = device.product_id;
    config.device.serial_number = device.serial_number;
    config.device.port_path = device.port_path;
    return open(config);
}

UsbBulkDevice::UsbBulkDevice(std::unique_ptr<Impl> implementation)
    : m_impl(std::move(implementation))
{
}

UsbBulkDevice::~UsbBulkDevice() = default;
UsbBulkDevice::UsbBulkDevice(UsbBulkDevice&&) noexcept = default;
UsbBulkDevice& UsbBulkDevice::operator=(UsbBulkDevice&&) noexcept = default;

tl::expected<void, std::error_code>
UsbBulkDevice::start_reads(ReadBufferProvider provider, ReadCallback callback)
{
    if (!m_impl)
    {
        return tl::make_unexpected(make_error_code(UsbError::DeviceDisconnected));
    }
    return m_impl->start_reads(std::move(provider), std::move(callback));
}

tl::expected<void, std::error_code> UsbBulkDevice::stop_reads()
{
    if (!m_impl)
    {
        return tl::make_unexpected(make_error_code(UsbError::DeviceDisconnected));
    }
    return m_impl->stop_reads();
}

void UsbBulkDevice::resume_reads()
{
    if (m_impl) m_impl->resume_reads();
}

tl::expected<void, std::error_code>
UsbBulkDevice::async_write_borrowed(std::span<const uint8_t> data, WriteCallback callback)
{
    if (!m_impl)
    {
        return tl::make_unexpected(make_error_code(UsbError::DeviceDisconnected));
    }
    return m_impl->async_write_borrowed(data, std::move(callback));
}

void UsbBulkDevice::on_disconnect(std::function<void(const std::error_code&)> callback)
{
    if (m_impl) m_impl->set_disconnect_callback(std::move(callback));
}

void UsbBulkDevice::on_reconnect(std::function<void()> callback)
{
    if (m_impl) m_impl->set_reconnect_callback(std::move(callback));
}

UsbState UsbBulkDevice::state() const noexcept
{
    return m_impl ? m_impl->state() : UsbState::Closed;
}

UsbBulkEndpointInfo UsbBulkDevice::endpoint_info() const noexcept
{
    return m_impl ? m_impl->endpoint_info() : UsbBulkEndpointInfo{};
}

UsbBulkStats UsbBulkDevice::stats() const noexcept
{
    return m_impl ? m_impl->stats() : UsbBulkStats{};
}

void UsbBulkDevice::close()
{
    if (m_impl) m_impl->close();
}
