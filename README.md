# Astrial - Astro Serial Library

一个基于 asio 的 跨平台 现代 C++ 串行通信库。支持设备信息获取等使用功能。

## Platform discovery

- Linux enumerates physical TTY devices through sysfs.
- macOS enumerates `IOSerialBSDClient` callout devices through IOKit and
  resolves USB product, manufacturer, serial number, VID, and PID metadata
  from the parent registry chain.
- Windows enumerates present COM ports through SetupAPI.

## Borrowed asynchronous I/O

The regular `on_data()` and `async_write()` APIs keep buffer ownership simple
by using Astrial-owned storage. Integrations that already own stable buffers can
avoid those user-space copies with the borrowed APIs:

```cpp
std::array<uint8_t, 1024> rx{};
bool rx_available = true;

serial.on_data_borrowed(
    [&]() -> std::span<uint8_t> {
        if (!rx_available) return {};
        rx_available = false;
        return rx;
    },
    [&](const std::error_code& error, std::size_t length) {
        consume(rx.data(), length, error);
        rx_available = true;
    });

serial.async_write_borrowed(tx_bytes, [](const std::error_code& error,
                                         std::size_t length) {
    // tx_bytes may be reused after this callback.
});
```

The read provider and completion callback execute on Astrial's I/O thread. An
empty span applies backpressure by pausing the read loop; call `resume_read()`
after storage becomes available. Every non-empty span is returned through its
completion callback, including close, cancellation, and disconnection paths.

Borrowed write data must remain alive and unchanged until its callback runs.
The existing `async_write()` API continues to make an owned copy.

## Optional USB bulk transport

Astrial can expose native USB bulk endpoints through an optional libusb-backed
target. The serial-only library remains dependency-free; enable this target
when configuring and link `astrial::usb`:

```bash
cmake -S . -B build-usb -DASTRIAL_BUILD_USB=ON -DASTRIAL_IO_URING=OFF
cmake --build build-usb
```

On Windows, Astrial's manifest keeps `libusb` scoped to the optional `usb`
feature. Configure from a Developer PowerShell with vcpkg's toolchain; CMake
installs the pinned dependency into the build tree and copies the matching DLL
beside built executables:

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
cmake -S . -B build-usb `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DVCPKG_MANIFEST_FEATURES=usb `
  -DASTRIAL_BUILD_USB=ON `
  -DASTRIAL_IO_URING=OFF
cmake --build build-usb --config Release --parallel
ctest --test-dir build-usb -C Release --output-on-failure
```

Use `x64-windows-static` instead when the application deliberately wants a
static libusb runtime. Astrial's imported target keeps Debug and Release
libraries separate for multi-configuration MSVC builds.

```cpp
UsbBulkConfig config;
config.device.vendor_id = 0x2fe3;
config.device.product_id = 0x574c;

auto device = UsbBulkDevice::open(config).value();
device.start_reads(
    acquire_rx_buffer,
    [](const std::error_code& error, UsbBorrowedBuffer buffer,
       std::size_t length) {
        consume_and_release(buffer.token, length, error);
    });
```

`UsbDeviceSelector` can additionally pin a device by serial number or physical
USB port path. Astrial validates that the selected interface exposes the
requested bulk endpoints before claiming it. `stop_reads()` synchronously
returns all borrowed read buffers; `endpoint_info()` and `stats()` expose the
negotiated packet sizes and transfer counters without adding callbacks to the
data path.

Two IN transfers are queued by default. Their payloads land directly in the
buffers returned by `acquire_rx_buffer`; returning an empty buffer applies
backpressure until `resume_reads()` is called. A token may identify the owner
of each borrowed span. Providers and completion callbacks execute on Astrial's
libusb event thread. The read provider/callback dispatch and single in-flight
write gate do not take a mutex on the transfer hot path; lifecycle operations
still synchronize `start_reads()`, `stop_reads()`, reconnect, and close.

`async_write_borrowed()` similarly keeps the caller's memory in place until
completion. Astrial sends an explicit zero-length packet when a non-empty OUT
transfer is an exact endpoint-packet multiple, including on platforms where
libusb's automatic ZLP flag is unavailable.

## Tests

On Linux, the integration test creates a raw pseudo-terminal and exercises both
owned and borrowed I/O without physical serial hardware:

```bash
cmake -S . -B build -DASTRIAL_BUILD_TESTS=ON -DASTRIAL_IO_URING=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Physical USB lifecycle checks are deliberately separate from CTest so a
developer machine without the selected device never fails ordinary CI. Build
the HIL tool explicitly, then point it at a Vendor Bulk interface:

```bash
cmake -S . -B build-usb-hil -DCMAKE_BUILD_TYPE=Release \
  -DASTRIAL_BUILD_USB=ON -DASTRIAL_BUILD_USB_HIL=ON \
  -DASTRIAL_BUILD_TESTS=ON -DASTRIAL_IO_URING=OFF
cmake --build build-usb-hil --target astrial_usb_lifecycle_hil
./build-usb-hil/tests/astrial_usb_lifecycle_hil \
  --vid 2fe3 --pid 574c --cycles 100
```

Each cycle verifies `open -> start_reads -> stop_reads -> restart -> close`,
duplicate/idempotent calls, post-close rejection, and exact return of every
borrowed RX buffer. Add `--wait-reconnect` to make each cycle wait for one
physical disconnect and re-enumeration; the versioned CSV phase line tells an
external fixture when it is safe to reset or power-cycle the device. The tool
then requires exactly one disconnect callback, one reconnect callback, and a
newly submitted read transfer before passing. Some Windows backends report one
`TransferStalled` completion when firmware removes the Bulk endpoint just
before disconnecting the device. Reconnect mode records and accepts at most one
such completion per read slot after the complete disconnect/reconnect sequence;
normal lifecycle mode still treats every stall as a failure.
