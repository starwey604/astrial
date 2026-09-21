# Astrial — 跨平台 C++20 设备 I/O 库

[English](README.md) | 简体中文

Astrial 是一个基于 asio 的 跨平台 现代 C++20 主机侧设备通信库，面向与嵌入式设备的通信场景。它提供类似 QSerialPort 的串口 API，并可通过可选的 libusb 后端直接收发 USB Bulk 端点数据；串口部分保持零第三方依赖，USB 传输按需启用。

面向嵌入式设备的现代 C++20 主机侧传输库。统一的 API 表面，两种传输方式：

- **串口（Serial）** — 基于 ASIO 的 QSerialPort 风格同步/异步 API，提供跨平台端口发现（VID/PID、厂商、序列号）。
- **USB Bulk** — 可选的 libusb 后端，直接访问原始端点，适用于厂商自定义协议，支持借用缓冲区（borrowed buffer）、背压与断线重连。

## 串口端口发现

- Linux 通过 sysfs 枚举物理 TTY 设备。
- macOS 通过 IOKit 枚举 `IOSerialBSDClient` callout 设备，并从父级注册表链解析 USB 产品、厂商、序列号、VID 与 PID 元数据。
- Windows 通过 SetupAPI 枚举当前存在的 COM 端口。

## 借用式异步 I/O

常规的 `on_data()` 与 `async_write()` API 通过 Astrial 自有的存储让缓冲区所有权保持简单。若集成方已经拥有稳定的缓冲区，可使用借用式 API 避免这些用户态拷贝：

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

读取提供者（provider）与完成回调都在 Astrial 的 I/O 线程上执行。返回空 span 会暂停读取循环以施加背压；待存储可用后调用 `resume_read()`。每个非空 span 都会通过其完成回调归还，包括关闭、取消和断线等路径。

借用的写入数据必须在其回调执行前保持存活且内容不变。现有的 `async_write()` API 仍会制作一份自有拷贝。

## 可选的 USB Bulk 传输

Astrial 可通过一个可选的、基于 libusb 的目标暴露原生 USB Bulk 端点。仅使用串口的库仍保持零依赖；在配置时启用该目标并链接 `astrial::usb`：

```bash
cmake -S . -B build-usb -DASTRIAL_BUILD_USB=ON -DASTRIAL_IO_URING=OFF
cmake --build build-usb
```

在 Windows 上，Astrial 的 manifest 将 `libusb` 限定在可选的 `usb` feature 内。请在 Developer PowerShell 中配合 vcpkg 工具链进行配置；CMake 会将锁定版本的依赖安装到构建树中，并把匹配的 DLL 复制到已构建的可执行文件旁边：

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

若应用有意使用静态 libusb 运行时，请改用 `x64-windows-static`。对于多配置的 MSVC 构建，Astrial 的 imported target 会分别保留 Debug 与 Release 库。

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

`UsbDeviceSelector` 还可以通过序列号或物理 USB 端口路径锁定设备。Astrial 在声明接口之前会校验所选接口确实暴露了请求的 Bulk 端点。`stop_reads()` 会同步归还所有借用的读取缓冲区；`endpoint_info()` 与 `stats()` 暴露协商后的包大小与传输计数器，且不会在数据路径上引入额外回调。计数器为可选项：使用 `-DASTRIAL_USB_STATS=ON` 配置才会启用，否则 `UsbBulkStats` 与 `stats()` 会被完全编译掉，热路径也不再执行原子更新。

默认会排队八个 IN 传输，可通过 `-DASTRIAL_USB_READ_QUEUE_DEPTH=N`（1..32）配置数量。它们的负载直接落在 `acquire_rx_buffer` 返回的缓冲区中；返回空缓冲区会施加背压，直到调用 `resume_reads()`。token 可用于标识每个借用 span 的归属。provider 与完成回调在 Astrial 的 libusb 事件线程上执行。读取的 provider/回调分发与单次在途写入闸门在传输热路径上不加锁；生命周期操作仍会对 `start_reads()`、`stop_reads()`、重连和关闭进行同步。

`async_write_borrowed()` 同样会保持调用方的内存在完成前原地不动。当一个非空 OUT 传输恰好是端点包大小的整数倍时，Astrial 会显式发送一个零长度包，即使在 libusb 自动 ZLP 标志不可用的平台上也是如此。

## 测试

在 Linux 上，集成测试会创建一个原始伪终端（pseudo-terminal），无需物理串口硬件即可同时测试自有缓冲区与借用式 I/O：

```bash
cmake -S . -B build -DASTRIAL_BUILD_TESTS=ON -DASTRIAL_IO_URING=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

物理 USB 生命周期检查刻意与 CTest 分离，这样即使开发者机器上没有目标设备，也不会导致常规 CI 失败。请显式构建 HIL 工具，然后将其指向一个 Vendor Bulk 接口：

```bash
cmake -S . -B build-usb-hil -DCMAKE_BUILD_TYPE=Release \
  -DASTRIAL_BUILD_USB=ON -DASTRIAL_BUILD_USB_HIL=ON \
  -DASTRIAL_BUILD_TESTS=ON -DASTRIAL_IO_URING=OFF
cmake --build build-usb-hil --target astrial_usb_lifecycle_hil
./build-usb-hil/tests/astrial_usb_lifecycle_hil \
  --vid 2fe3 --pid 574c --cycles 100
```

每个循环都会验证 `open -> start_reads -> stop_reads -> restart -> close`、重复/幂等调用、关闭后的拒绝，以及每个借用 RX 缓冲区的精确归还。添加 `--wait-reconnect` 可让每个循环等待一次物理断开与重新枚举；带版本号的 CSV 阶段行会告知外部夹具（fixture）何时可以安全地复位或断电重启设备。该工具随后要求恰好一次断开回调、一次重连回调，以及一次新提交的读取传输，才能判定通过。某些 Windows 后端会在固件于断开设备前移除 Bulk 端点时上报一次 `TransferStalled` 完成事件。重连模式会在完整的断开/重连序列之后，为每个读取槽记录并接受至多一次这样的完成事件；常规生命周期模式仍将任何 stall 视为失败。
