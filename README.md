# Astrial - Astro Serial Library

一个基于 asio 的 跨平台 现代 C++ 串行通信库。支持设备信息获取等使用功能。

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

## Tests

On Linux, the integration test creates a raw pseudo-terminal and exercises both
owned and borrowed I/O without physical serial hardware:

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DASTRIAL_IO_URING=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```
