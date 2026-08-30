/* SPDX-License-Identifier: MIT */

#ifndef ASTRIAL_USB_HPP
#define ASTRIAL_USB_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <tl/expected.hpp>

enum class UsbError
{
    Success = 0,
    DeviceNotFound,
    PermissionDenied,
    InterfaceBusy,
    InvalidArgument,
    DeviceDisconnected,
    TransferTimeout,
    TransferCancelled,
    TransferStalled,
    TransferOverflow,
    NotSupported,
    IoError,
};

const std::error_category& get_usb_category();
std::error_code make_error_code(UsbError error);

struct UsbDeviceInfo
{
    uint16_t vendor_id{};
    uint16_t product_id{};
    uint8_t bus_number{};
    uint8_t device_address{};
    std::vector<uint8_t> port_path;
    std::string manufacturer;
    std::string product;
    std::string serial_number;
};

struct UsbDeviceSelector
{
    uint16_t vendor_id{};
    uint16_t product_id{};
    std::string serial_number;
    std::vector<uint8_t> port_path;
};

struct UsbBulkInterface
{
    uint8_t interface_number{};
    uint8_t endpoint_in{0x81};
    uint8_t endpoint_out{0x01};
};

struct UsbBulkConfig
{
    UsbDeviceSelector device;
    UsbBulkInterface bulk_interface;
    std::size_t read_queue_depth{2};
    bool auto_detach_kernel_driver{true};
    bool auto_reconnect{true};
    std::chrono::milliseconds reconnect_interval{500};
};

struct UsbBulkEndpointInfo
{
    uint16_t maximum_packet_size_in{};
    uint16_t maximum_packet_size_out{};
};

struct UsbBulkStats
{
    uint64_t reads_submitted{};
    uint64_t reads_completed{};
    uint64_t bytes_received{};
    uint64_t writes_submitted{};
    uint64_t writes_completed{};
    uint64_t bytes_transmitted{};
    uint64_t cancellations{};
    uint64_t errors{};
    uint64_t reconnects{};
};

struct UsbBorrowedBuffer
{
    std::span<uint8_t> bytes;
    std::uintptr_t token{};
};

enum class UsbState { Disconnected, Connected, Reconnecting, Closed };

class UsbBulkDevice
{
public:
    using ReadBufferProvider = std::function<UsbBorrowedBuffer()>;
    using ReadCallback =
        std::function<void(const std::error_code&, UsbBorrowedBuffer, std::size_t)>;
    using WriteCallback = std::function<void(const std::error_code&, std::size_t)>;

    static tl::expected<std::vector<UsbDeviceInfo>, std::error_code> list_devices();
    static tl::expected<UsbBulkDevice, std::error_code> open(const UsbBulkConfig& config);
    static tl::expected<UsbBulkDevice, std::error_code>
    open(const UsbDeviceInfo& device, UsbBulkConfig config);

    ~UsbBulkDevice();
    UsbBulkDevice(const UsbBulkDevice&) = delete;
    UsbBulkDevice& operator=(const UsbBulkDevice&) = delete;
    UsbBulkDevice(UsbBulkDevice&&) noexcept;
    UsbBulkDevice& operator=(UsbBulkDevice&&) noexcept;

    tl::expected<void, std::error_code> start_reads(ReadBufferProvider provider,
                                                    ReadCallback callback);
    // Stops new reads and synchronously returns every submitted borrowed
    // buffer. Do not call from a USB callback.
    tl::expected<void, std::error_code> stop_reads();
    // Wake the event thread after a provider previously returned an empty
    // buffer. Providers and callbacks run on that event thread.
    void resume_reads();

    // The caller owns data until callback completion. Only one write may be
    // active; InterfaceBusy means the request was not accepted.
    tl::expected<void, std::error_code>
    async_write_borrowed(std::span<const uint8_t> data, WriteCallback callback);

    void on_disconnect(std::function<void(const std::error_code&)> callback);
    void on_reconnect(std::function<void()> callback);
    [[nodiscard]] UsbState state() const noexcept;
    [[nodiscard]] UsbBulkEndpointInfo endpoint_info() const noexcept;
    [[nodiscard]] UsbBulkStats stats() const noexcept;
    // May be called from a callback. The object itself must be destroyed from
    // another thread so its event thread can be joined safely.
    void close();

private:
    class Impl;
    explicit UsbBulkDevice(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> m_impl;
};

template <>
struct std::is_error_code_enum<UsbError> : std::true_type
{
};

#endif // ASTRIAL_USB_HPP
