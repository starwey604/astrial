# Astrial — Agent Guide

Minimal cross-platform C++20 serial library built on ASIO.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

- Requires CMake >= 3.22, C++20 compiler.
- `BUILD_EXAMPLE` ON by default; pass `-DBUILD_EXAMPLE=OFF` to skip examples.
- Buffer sizes: `-DASTRIAL_READ_BUFFER_SIZE=<bytes>` / `-DASTRIAL_WRITE_BUFFER_SIZE=<bytes>` (default 4096), exposed as `ASTRIAL_READ_BUFFER_LENGTH` / `ASTRIAL_WRITE_BUFFER_LENGTH`.
- Vendored deps: `3rdparty/asio`, `3rdparty/tl-expected`, `3rdparty/readerwriterqueue` — no package manager needed.
- `-DASTRIAL_IO_URING=OFF` to skip io_uring detection. Auto-detected on Linux (kernel >= 5.15 + liburing), falls back to epoll.
- Windows: links `setupapi` + `cfgfmgr32` automatically. LTO enabled by default.

## No tests / no CI / no linter

- `test/` is empty.
- No formatter, linter, or typechecker config exists.
- No CI workflows.

## Architecture

| Path | Role |
|---|---|
| `include/astrial.hpp` | Umbrella header |
| `include/astrial/*.hpp` | Public API: `Serial`, `SerialBuilder`, `Types`, `port.hpp` |
| `src/astrial/*.cpp` | Implementation matching headers |
| `src/astrail.cpp` | Single `#include <astrial.hpp>` (note: `astrail` vs `astrial`) |
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

## Platform notes

- Linux port enumeration reads `/sys/class/tty` and walks the USB device tree for VID/PID/manufacturer/serial.
- macOS port enumeration uses the **same `/sys/class/tty` Linux code path** — will not work. Needs IOKit-based implementation.
- Windows port enumeration uses SetupAPI (`setupapi.lib`).

## Source typo

`src/astrail.cpp` is intentionally named `astrail` (missing the second `i`). This is not a mistake.

## `.gitignore`

Ignores `cmake-build-*`, `.idea`, `build`. Use `-B <dir>` outside these patterns.
