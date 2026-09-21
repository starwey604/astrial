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

// Default number of in-flight IN transfers. The ASTRIAL_USB_READ_QUEUE_DEPTH
// CMake option (1..32) overrides this value.
#if !defined(ASTRIAL_USB_READ_QUEUE_DEPTH)
#  define ASTRIAL_USB_READ_QUEUE_DEPTH 8
#endif

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
    std::size_t read_queue_depth{ASTRIAL_USB_READ_QUEUE_DEPTH};
    bool auto_detach_kernel_driver{true};
    bool auto_reconnect{true};
    std::chrono::milliseconds reconnect_interval{500};
};

struct UsbBulkEndpointInfo
{
    uint16_t maximum_packet_size_in{};
    uint16_t maximum_packet_size_out{};
};

#if defined(ASTRIAL_USB_STATS) && ASTRIAL_USB_STATS
// Only available when astrial::usb is built with ASTRIAL_USB_STATS=1.
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
#endif

struct UsbBorrowedBuffer
{
    std::span<uint8_t> bytes;
    std::uintptr_t token{};
};

// Configuration for the library-owned read path. Astrial allocates
// read_queue_depth buffers of buffer_size bytes each; every allocation is
// rounded up to the platform page size so DMA paths avoid a bounce buffer.
struct UsbReadBufferConfig
{
    std::size_t buffer_size{64 * 1024};
};

// Reports the library-owned read path. All fields are zero when reads use the
// borrowed path or have not started. `zero_copy` is true when the buffers came
// from libusb DMA memory (`libusb_dev_mem_alloc`), in which case the host
// controller writes into them directly.
struct UsbReadBufferInfo
{
    std::size_t buffer_size{};
    std::size_t depth{};
    bool page_aligned{};
    bool zero_copy{};
};

enum class UsbState { Disconnected, Connected, Reconnecting, Closed };

class UsbBulkDevice
{
public:
    using ReadBufferProvider = std::function<UsbBorrowedBuffer()>;
    using ReadCallback =
        std::function<void(const std::error_code&, UsbBorrowedBuffer, std::size_t)>;
    using OwnedReadCallback =
        std::function<void(const std::error_code&, std::span<const uint8_t>)>;
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

    // Starts reads into library-owned, page-aligned buffers. `data` is only
    // valid until the callback returns; copy it to retain. No provider and no
    // backpressure bookkeeping are needed.
    tl::expected<void, std::error_code> start_reads(const UsbReadBufferConfig& config,
                                                    OwnedReadCallback callback);
    // Starts reads into caller-owned buffers supplied by `provider`. The
    // provider must return a distinct, live buffer for every free slot.
    tl::expected<void, std::error_code> start_reads_borrowed(ReadBufferProvider provider,
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
    [[nodiscard]] UsbReadBufferInfo read_buffer_info() const noexcept;
#if defined(ASTRIAL_USB_STATS) && ASTRIAL_USB_STATS
    [[nodiscard]] UsbBulkStats stats() const noexcept;
#endif
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
