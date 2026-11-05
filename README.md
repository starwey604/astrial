# Astrial — Cross-Platform C++20 Device I/O Library

[English](README.md) | [简体中文](README.zh-CN.md)

Astrial 是一个基于 asio 的 跨平台 现代 C++20 主机侧设备通信库，面向与嵌入式设备的通信场景。它提供类似 QSerialPort 的串口 API，并可通过可选的 libusb 后端直接收发 USB Bulk 端点数据；串口部分保持零第三方依赖，USB 传输按需启用。

A modern C++20 host-side transport library for embedded devices. One API
surface, two transports:

- **Serial** — QSerialPort-style synchronous/async API on top of ASIO, with
  cross-platform port discovery (VID/PID, manufacturer, serial number).
- **USB bulk** — optional libusb-backed raw endpoint access for vendor-specific
  protocols, with borrowed buffers, backpressure, and reconnect handling.

## Serial port discovery

- Linux enumerates physical TTY devices through sysfs.
- macOS enumerates `IOSerialBSDClient` callout devices through IOKit and
  resolves USB product, manufacturer, serial number, VID, and PID metadata
  from the parent registry chain.
- Windows enumerates present COM ports through SetupAPI.

## Serial configuration and runtime control

```cpp
auto serial = Serial::builder()
    .baud_rate(115200)
    .data_bits(DataBits::Eight)
    .parity(Parity::None)
    .stop_bits(StopBits::One)
    .flow_control(FlowControl::Hardware) // None | Software | Hardware
    .auto_reconnect(true)
    .open("/dev/ttyACM0")
    .value();

serial.set_baud_rate(921600); // reconfigures the open port in place
serial.set_flow_control(FlowControl::None);
if (serial.is_open()) { /* state() == SerialState::Connected */ }
```

The runtime setters return `tl::expected<void, std::error_code>`, run on the I/O
thread, and update the stored settings so an automatic reconnect reopens with
them. `state()` and `is_open()` report whether the port is connected or
reconnecting.

## RS-232 control lines and BREAK

For a modem, reset line, or a full DB9 test fixture, use the open `Serial`
object directly. These controls follow QSerialPort's logical signal meanings:
`true` asserts a control line; it does not mean a high voltage on an MCU-side
TTL pin. All methods below return `tl::expected<..., std::error_code>`.

```cpp
serial.set_data_terminal_ready(true).value();
serial.set_request_to_send(false).value(); // requires non-hardware flow control

auto pins = serial.pinout_signals().value();
bool cts = has_signal(pins, PinoutSignals::ClearToSend);
bool dsr = has_signal(pins, PinoutSignals::DataSetReady);
bool dcd = has_signal(pins, PinoutSignals::DataCarrierDetect);
bool ri  = has_signal(pins, PinoutSignals::RingIndicator);
```

`is_data_terminal_ready()` and `is_request_to_send()` also query the driver,
returning `expected<bool, error_code>`. `pinout_signals()` returns all six DB9
control signals, including the DTR/RTS outputs. A failed query returns an error,
not an empty signal mask. Unsupported driver operations return
`SerialError::UnsupportedOperation`; other native errors are preserved.

Optional builder settings `.data_terminal_ready(bool)` and
`.request_to_send(bool)` apply after opening; an OS/adapter may briefly apply
its defaults during open. Only explicitly requested, successfully set outputs
are restored before the reconnect callback. Existing callers do not need modem
control support. Hardware flow control owns RTS: a manual RTS setter, or an
explicit builder RTS setting combined with `FlowControl::Hardware`, fails with
`UnsupportedOperation`. Enabling hardware flow control discards saved manual
RTS intent; set RTS again after disabling it if a particular state is needed.
DTR remains independent of RTS/CTS flow control.

For a persistent BREAK use `set_break_enabled(true)` and later
`set_break_enabled(false)`. `is_break_enabled()` reports Astrial's successfully
applied local state; there is no portable physical BREAK readback. For a timed
BREAK that leaves the I/O thread responsive:

```cpp
serial.async_send_break(std::chrono::milliseconds(250),
    [](const std::error_code& error) {
        if (error) std::cerr << "BREAK: " << error.message() << '\n';
    });
```

`send_break(duration)` offers a blocking caller-side equivalent (default
250 ms). Use the async form inside I/O callbacks; the blocking form returns
`resource_deadlock_would_occur` there. Durations must be positive. BREAK does
not drain queued TX, and timing is subject to host/USB scheduling. Another BREAK
request during a pulse, or a pulse while persistent BREAK is active, returns
`operation_in_progress`. Closing/disconnecting cancels a pulse, completes its
callback once, and clears local BREAK state. BREAK is never replayed on
reconnect. Accepted async callbacks run on the I/O thread; a request rejected
before enqueueing (closed/moved-from object) invokes its callback on the caller.
Keep the object alive until callbacks finish; destroy it outside its I/O thread.

To observe sampled modem-state changes, opt into polling (off by default):

```cpp
serial.on_pinout_changed(
    [](const std::error_code& error, PinoutSignals pins) {
        if (error) {
            std::cerr << "Pinout: " << error.message() << '\n';
            return;
        }
        std::cout << "CTS asserted: " << has_signal(pins, PinoutSignals::ClearToSend) << '\n';
    }, std::chrono::milliseconds(20)).value();
// Later: serial.on_pinout_changed({}).value();
```

Registration queries a baseline without emitting it; subsequent changes are
reported on the I/O thread. Short pulses can be missed, so this is not edge
capture. An error is delivered once and stops polling; re-register to retry.
A detected disconnect cancels polling; reconnect starts a fresh baseline and
resumes the subscription. As with ordinary serial I/O, automatic disconnect
recovery requires an active read/write operation to detect a lost device.
Callbacks may query/set controls, unregister monitoring, or call `close()`.

### Platform coverage and QSerialPort correspondence

| QSerialPort control | Astrial | Platforms |
| --- | --- | --- |
| `setDataTerminalReady` / `isDataTerminalReady` | `set_data_terminal_ready` / `is_data_terminal_ready` | Linux, macOS, Windows; driver-dependent |
| `setRequestToSend` / `isRequestToSend` | `set_request_to_send` / `is_request_to_send` | Linux, macOS, Windows; hardware-flow restriction |
| `pinoutSignals` | `pinout_signals` + `PinoutSignals` / `has_signal` | Six DB9 control signals; driver-dependent |
| `setBreakEnabled` / `isBreakEnabled` | `set_break_enabled` / `is_break_enabled` | Linux, macOS, Windows; driver-dependent |
| `sendBreak` | `send_break` / `async_send_break` | Host-timed pulse; driver-dependent |
| Mark / Space parity | `Parity::Mark` / `Parity::Space` | Windows and Linux with `CMSPAR`; unsupported on macOS |

`line_error_counters()` is an additional Linux `TIOCGICOUNT` snapshot of parity,
framing, overrun, BREAK, and buffer-overrun counters. An unsupported driver
(including adapters that do not implement this ioctl), Windows, or macOS returns
`UnsupportedOperation`. Counters may wrap or reset on reopen and USB drivers may
count error reports rather than individual corrupt bytes. They do not provide
per-byte RX error/BREAK callbacks. The existing ASIO byte-stream callbacks do
not expose those flags. RS-485 direction configuration is a separate capability
and is not implemented by these RS-232 controls.

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

// Astrial can own the read buffers and hand you a view of the payload:
device.start_reads({.buffer_size = 64 * 1024},
    [](const std::error_code& error, std::span<const uint8_t> data) {
        // `data` is valid until the callback returns; copy it to retain.
        consume(error, data);
    });

// Or supply and own stable buffers yourself (no copy into the app):
device.start_reads_borrowed(
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
data path. The counters are opt-in: configure with `-DASTRIAL_USB_STATS=ON`,
otherwise `UsbBulkStats` and `stats()` are compiled out and the hot path skips
the atomic updates entirely.

Eight IN transfers are queued by default; configure the count with
`-DASTRIAL_USB_READ_QUEUE_DEPTH=N` (1..32). The library-owned `start_reads()`
allocates page-aligned buffers itself, so the DMA path avoids a bounce buffer.
Where the kernel offers usbfs DMA memory, Astrial takes the buffers from
`libusb_dev_mem_alloc` and the host controller writes into them directly
(zero-copy); `read_buffer_info()` reports the active mode and the buffers are
reallocated across reconnects.
With `start_reads_borrowed()` the payloads land directly in the buffers returned
by `acquire_rx_buffer`; returning an empty buffer applies backpressure until
`resume_reads()` is called. A token may identify the owner of each borrowed
span. Providers and completion callbacks execute on Astrial's libusb event
thread. The read provider/callback dispatch and single in-flight write gate do
not take a mutex on the transfer hot path; lifecycle operations still
synchronize `start_reads()`/`start_reads_borrowed()`, `stop_reads()`, reconnect,
and close.

For borrowed reads, `UsbReadBufferPool` allocates a reusable set of page-aligned
buffers and tracks which are free, so the provider always hands out a distinct
live buffer:

```cpp
auto pool = UsbReadBufferPool::create(64 * 1024, 8).value();

device.start_reads_borrowed(
    [&] { return pool.acquire(); },
    [&](const std::error_code& error, UsbBorrowedBuffer buffer,
        std::size_t length) {
        consume(buffer, length, error);
        pool.release(buffer); // returns it to the free list
    });
```

`acquire()` returns an empty buffer when every buffer is checked out, which
applies the read loop's backpressure automatically.

`async_write_borrowed()` similarly keeps the caller's memory in place until
completion. By default it sends exactly the payload. Some device protocols use
a short packet to end a logical message; for those, request a zero-length
packet when the payload is an exact endpoint-packet multiple:

```cpp
device.async_write_borrowed(bytes, on_complete,
                            {.append_zero_length_packet = true});
```

This choice belongs to the device protocol. Astrial submits the extra packet
explicitly so the option works across libusb backends.

## Tests

On Linux, the integration test creates a raw pseudo-terminal and exercises both
owned and borrowed I/O without physical serial hardware:

```bash
cmake -S . -B build -DASTRIAL_BUILD_TESTS=ON -DASTRIAL_IO_URING=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

The PTY tests cover transport behavior and explicit unsupported-control errors.
Linux static builds additionally run linker-boundary native-driver response tests for control
mapping, errors, timers, callbacks, and reconnect restoration. Those tests do not
validate electrical behavior.

The opt-in `tests/serial_hil.cpp` suite validates a physical RS-232 loopback:
disconnect other equipment and wire DB9 **2-3**, **7-8**, and **4-1-6-9**. It checks
binary data and all DTR/RTS output combinations against CTS/DSR/DCD/RI.

```bash
cmake -S . -B build -DASTRIAL_BUILD_TESTS=ON -DASTRIAL_BUILD_HIL_TESTS=ON
cmake --build build
ASTRIAL_HIL_PORT=/dev/ttyUSB0 ASTRIAL_HIL_FIXTURE=rs232-loopback \
  ctest --test-dir build -R '^astrial_serial_hil$' --output-on-failure
```

Without both environment variables it skips (exit 77). Do not use loopback mode
while an MCU or other RS-232 peer is attached. Independent frame-format checks,
actual hardware-flow throttling, BREAK waveform/detection, error injection,
and physical reconnect still require a peer/instrument fixture; the tool reports
these as not tested rather than claiming a full HIL pass. RS-485 remains separate.

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
