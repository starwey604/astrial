# Astrial — Agent Guide

Minimal cross-platform C++20 host-side transport library built on ASIO.
Covers serial ports (zero-dependency core) and optional libusb-backed USB bulk
endpoints (`astrial::usb`).

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

- Requires CMake >= 3.22, C++20 compiler.
- `ASTRIAL_BUILD_EXAMPLES` and `ASTRIAL_BUILD_TESTS` default ON only when
  Astrial is the top-level project. `BUILD_EXAMPLE` remains a compatibility
  input for older standalone build commands.
- Buffer sizes: `-DASTRIAL_READ_BUFFER_SIZE=<bytes>` / `-DASTRIAL_WRITE_BUFFER_SIZE=<bytes>` (default 4096), exposed as `ASTRIAL_READ_BUFFER_LENGTH` / `ASTRIAL_WRITE_BUFFER_LENGTH`.
- Vendored deps: `3rdparty/asio`, `3rdparty/tl-expected`, `3rdparty/readerwriterqueue` — no package manager needed.
- `-DASTRIAL_IO_URING=OFF` to skip io_uring detection. Auto-detected on Linux (kernel >= 5.15 + liburing), falls back to epoll.
- Optional USB bulk: `-DASTRIAL_BUILD_USB=ON` (default OFF) builds the
  `astrial::usb` target from `src/astrial/Usb.cpp` and requires `LibUSB 1.0`.
  The serial core (`astrial`) never depends on it. On Windows use the vcpkg
  manifest feature `usb` (`vcpkg.json`, pinned baseline) instead of a global
  libusb install. The target links `LibUSB::LibUSB` and sets `ASTRIAL_HAS_USB=1`
  PUBLIC, which is what makes `astrial.hpp` expose `Usb.hpp`.
- `-DASTRIAL_BUILD_USB_HIL=ON` builds the physical USB lifecycle test tool; it
  requires `-DASTRIAL_BUILD_USB=ON` and is not registered with CTest.
- `-DASTRIAL_USB_READ_QUEUE_DEPTH=<n>` (default 8, range 1..32) sets the default
  `UsbBulkConfig::read_queue_depth` (in-flight USB bulk IN transfers); it is a
  PUBLIC definition on `astrial::usb` because the header uses it.
- `-DASTRIAL_USB_STATS=ON` exposes `UsbBulkStats` / `UsbBulkDevice::stats()` and
  collects counters; it is OFF by default, in which case the counters and the
  public API are compiled out and the hot path does no atomic RMW. The
  `ASTRIAL_USB_STATS=1` definition is PUBLIC on `astrial::usb`, and building with
  `-DASTRIAL_BUILD_USB_HIL=ON` forces it on because the HIL tool asserts on stats.
- Windows: links `setupapi` + `cfgmgr32` automatically for serial enumeration.
  IPO/LTO is target-local and enabled only for optimized configurations by
  default.

## Tests and CI

- CTest always runs a `Serial::list_ports()` smoke test.
- Linux and macOS additionally run end-to-end serial I/O tests over a pseudo-terminal.
- When the USB target is built, CTest also runs `astrial_usb_smoke`.
- USB lifecycle HIL (`tests/usb_lifecycle_hil.cpp`) is only built with
  `ASTRIAL_BUILD_USB_HIL=ON` and is never registered with CTest — run it
  manually against a real Vendor Bulk device so ordinary CI never needs hardware.
- `.github/workflows/ci.yml` builds examples and runs tests on Linux, macOS, and
  Windows, enabling `-DASTRIAL_BUILD_USB=ON` on all three (libusb from apt/brew,
  vcpkg manifest feature on Windows).
- No formatter, linter, or typechecker config exists.

## Architecture

| Path | Role |
|---|---|
| `include/astrial.hpp` | Umbrella header; includes `Usb.hpp` when `ASTRIAL_HAS_USB` is defined |
| `include/astrial/*.hpp` | Public API: `Serial`, `SerialBuilder`, `Types`, `port.hpp`, optional `Usb.hpp` |
| `src/astrial/*.cpp` | Implementation matching headers |
| `src/astrial/Usb.cpp` | libusb bulk backend; compiled only into the optional `astrial::usb` target |
| `src/astrial.cpp` | Single `#include <astrial.hpp>` |
| `examples/` | Four example executables: `01_connect`, `01_list_ports`, `02_reconnect`, `02_async_write` (link `astrial`) |

### API usage pattern

```cpp
auto serial = Serial::builder()
    .baud_rate(115200)
    .parity(Parity::None)
    .stop_bits(StopBits::One)
    .open("/dev/ttyACM0")
    .or_else([](auto&& e) { throw std::runtime_error(e.message()); })
    .value();

serial.on_data([](std::span<const uint8_t> data) { /* async callback */ });
serial.write(cmd); // synchronous write
```

- `Serial` is move-only (delete copy). Uses PIMPL with background `asio::io_context` thread.
- `SerialBuilder` returns `tl::expected<Serial, std::error_code>`.
- Builder supports `baud_rate`, `parity`, `stop_bits`, `data_bits`, `auto_reconnect`.
- Port listing: `Serial::list_ports()` returns `std::vector<SerialInfo>`.
- `SerialBuilder::open()` has two overloads: by name (`std::string_view`, e.g. `"/dev/ttyACM0"`) or by `SerialInfo`.
- `write()` is synchronous but returns `tl::expected<void, std::error_code>` (not `void`). `async_write()` exists for async.
- Callbacks: `on_data`, `on_disconnect`, `on_reconnect`. `close()` tears down the connection.

### USB bulk API (`astrial::usb`, `ASTRIAL_HAS_USB`)

```cpp
UsbBulkConfig config;
config.device.vendor_id = 0x2fe3;
config.device.product_id = 0x574c;

auto device = UsbBulkDevice::open(config).value();
device.start_reads(acquire_rx_buffer, on_read);
```

- `UsbBulkDevice` is move-only; `open()` and `list_devices()` return
  `tl::expected<..., std::error_code>`.
- Reads are borrowed-only: the provider returns a `UsbBorrowedBuffer` (a `span`
  plus an opaque `token`), and `stop_reads()` synchronously returns every
  outstanding buffer.
- `async_write_borrowed()` is the only write path; one write may be in flight
  and the caller's memory must stay alive until its callback.
- Providers and callbacks run on the libusb event thread. `resume_reads()` is
  needed after a provider returns an empty buffer (backpressure).
- `UsbDeviceSelector` can pin a device by VID/PID plus serial number or physical
  port path; `endpoint_info()`, `stats()` (only when `ASTRIAL_USB_STATS=ON`),
  `state()`, `on_disconnect`, and `on_reconnect` round out the surface.
  `close()` may be called from a callback, but the object must be destroyed from
  another thread.

## Platform notes

- Linux port enumeration reads `/sys/class/tty` and walks the USB device tree for VID/PID/manufacturer/serial.
- macOS port enumeration uses IOKit's `IOSerialBSDClient` services and returns
  callout paths. USB metadata is resolved by searching the parent registry
  chain; non-USB ports fall back to their TTY device description.
- Windows port enumeration uses SetupAPI (`setupapi.lib`).

## `.gitignore`

Ignores `cmake-build-*`, `.idea`, `build`. Use `-B <dir>` outside these patterns.
