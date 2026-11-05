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

## 串口配置与运行时控制

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

serial.set_baud_rate(921600); // 就地重配置已打开的端口
serial.set_flow_control(FlowControl::None);
if (serial.is_open()) { /* state() == SerialState::Connected */ }
```

运行时 setter 返回 `tl::expected<void, std::error_code>`，在 I/O 线程上执行，并会更新已保存的设置，使自动重连能够以新配置重新打开端口。`state()` 与 `is_open()` 用于查询端口当前处于已连接还是重连中。

## RS-232 控制线与 BREAK

需要控制调制解调器、复位线或完整 DB9 测试夹具时，可直接使用已打开的 `Serial`。
这些接口沿用 QSerialPort 的逻辑含义：`true` 表示控制信号有效，不表示 MCU 侧 TTL
引脚电压为高。下列方法均返回 `tl::expected<..., std::error_code>`。

```cpp
serial.set_data_terminal_ready(true).value();
serial.set_request_to_send(false).value(); // 必须未启用硬件流控

auto pins = serial.pinout_signals().value();
bool cts = has_signal(pins, PinoutSignals::ClearToSend);
bool dsr = has_signal(pins, PinoutSignals::DataSetReady);
bool dcd = has_signal(pins, PinoutSignals::DataCarrierDetect);
bool ri  = has_signal(pins, PinoutSignals::RingIndicator);
```

`is_data_terminal_ready()` 和 `is_request_to_send()` 同样查询驱动，返回
`expected<bool, error_code>`。`pinout_signals()` 返回六路 DB9 控制信号，包括 DTR/RTS
输出。查询失败会返回错误，而不是空位图。驱动不支持时返回
`SerialError::UnsupportedOperation`，其他系统错误保留原错误码。

Builder 可选的 `.data_terminal_ready(bool)` 与 `.request_to_send(bool)` 在打开后应用；
打开瞬间仍可能短暂出现系统或转接器的默认状态。只有显式请求且设置成功的输出才会在
重连回调之前恢复，旧调用方不需要支持控制线。硬件流控接管 RTS：手动设置 RTS，或
同时配置 Builder 的 RTS 与 `FlowControl::Hardware`，都会返回 `UnsupportedOperation`。
启用硬件流控会放弃保存的手动 RTS 设置；关闭流控后若需要特定状态，请重新设置 RTS。
DTR 不受 RTS/CTS 流控影响。

持续 BREAK 使用 `set_break_enabled(true)`，结束时调用 `set_break_enabled(false)`。
`is_break_enabled()` 返回 Astrial 已成功应用的本地状态，不是物理波形读回。
定时 BREAK 可异步发送，不阻塞 I/O 线程：

```cpp
serial.async_send_break(std::chrono::milliseconds(250),
    [](const std::error_code& error) {
        if (error) std::cerr << "BREAK: " << error.message() << '\n';
    });
```

`send_break(duration)` 提供阻塞调用线程的版本，默认 250 ms。在 I/O 回调中应使用
异步版本；同步版本在该线程返回 `resource_deadlock_would_occur`。时长必须为正数。
BREAK 不会等待已有 TX 排空，实际时序受主机与 USB 调度影响。脉冲期间再次操作 BREAK，
或持续 BREAK 尚未结束时请求脉冲，返回 `operation_in_progress`。关闭或断线会取消脉冲，
完成其回调一次并清除本地 BREAK 状态；重连不会重新发送 BREAK。已入队请求的回调在
I/O 线程执行；入队前因关闭或对象已移走而拒绝的请求，在调用线程执行回调。对象须存活
到回调结束，并从 I/O 线程以外销毁。

如需观察控制线变化，可显式启用轮询；默认不开启：

```cpp
serial.on_pinout_changed(
    [](const std::error_code& error, PinoutSignals pins) {
        if (error) {
            std::cerr << "Pinout: " << error.message() << '\n';
            return;
        }
        std::cout << "CTS asserted: " << has_signal(pins, PinoutSignals::ClearToSend) << '\n';
    }, std::chrono::milliseconds(20)).value();
// 停止监听：serial.on_pinout_changed({}).value();
```

注册时查询基线但不发送初始通知，后续变化在 I/O 线程回调。短脉冲可能漏采，不能用作
边沿捕获。查询错误只通知一次并停止轮询，可重新注册以重试。检测到断线后取消轮询，
重连时建立新基线并恢复订阅。与原有串口 I/O 一样，自动断线恢复需要活动的读写操作来
检测设备丢失。回调内可查询或设置控制线、取消监听以及调用 `close()`。

### 平台覆盖与 QSerialPort 对照

| QSerialPort 控制项 | Astrial | 平台 |
| --- | --- | --- |
| `setDataTerminalReady` / `isDataTerminalReady` | `set_data_terminal_ready` / `is_data_terminal_ready` | Linux、macOS、Windows，取决于驱动 |
| `setRequestToSend` / `isRequestToSend` | `set_request_to_send` / `is_request_to_send` | Linux、macOS、Windows；硬件流控时不能手动设置 |
| `pinoutSignals` | `pinout_signals` + `PinoutSignals` / `has_signal` | 六路 DB9 控制信号，取决于驱动 |
| `setBreakEnabled` / `isBreakEnabled` | `set_break_enabled` / `is_break_enabled` | Linux、macOS、Windows，取决于驱动 |
| `sendBreak` | `send_break` / `async_send_break` | 主机计时脉冲，取决于驱动 |
| Mark / Space 校验 | `Parity::Mark` / `Parity::Space` | Windows、带 `CMSPAR` 的 Linux；macOS 不支持 |

额外提供的 `line_error_counters()` 通过 Linux `TIOCGICOUNT` 查询校验错、帧错、溢出、
BREAK 和缓冲区溢出的累计快照。驱动未实现此操作时，以及在 Windows、macOS 上，返回
`UnsupportedOperation`。计数器可能回绕或在重新打开时重置，USB 驱动也可能按错误报告
而非损坏字节计数。它不是逐字节的接收错误或 BREAK 通知；现有 ASIO 字节流回调不暴露
这些标记。RS-485 方向配置是另一项能力，本次 RS-232 控制接口不包含该配置。

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

// 由 Astrial 拥有读取缓冲区，并交给你一个负载视图：
device.start_reads({.buffer_size = 64 * 1024},
    [](const std::error_code& error, std::span<const uint8_t> data) {
        // `data` 仅在回调返回前有效；需要保留请自行拷贝。
        consume(error, data);
    });

// 也可以自行提供并持有稳定缓冲区（避免向应用内拷贝）：
device.start_reads_borrowed(
    acquire_rx_buffer,
    [](const std::error_code& error, UsbBorrowedBuffer buffer,
       std::size_t length) {
        consume_and_release(buffer.token, length, error);
    });
```

`UsbDeviceSelector` 还可以通过序列号或物理 USB 端口路径锁定设备。Astrial 在声明接口之前会校验所选接口确实暴露了请求的 Bulk 端点。`stop_reads()` 会同步归还所有借用的读取缓冲区；`endpoint_info()` 与 `stats()` 暴露协商后的包大小与传输计数器，且不会在数据路径上引入额外回调。计数器为可选项：使用 `-DASTRIAL_USB_STATS=ON` 配置才会启用，否则 `UsbBulkStats` 与 `stats()` 会被完全编译掉，热路径也不再执行原子更新。

默认会排队八个 IN 传输，可通过 `-DASTRIAL_USB_READ_QUEUE_DEPTH=N`（1..32）配置数量。库自有的 `start_reads()` 会自行分配页对齐缓冲区，从而让 DMA 路径不需要 bounce buffer。在内核提供 usbfs DMA 内存的平台上，Astrial 会通过 `libusb_dev_mem_alloc` 取用缓冲区，主机控制器直接写入其中（零拷贝）；`read_buffer_info()` 会报告当前实际模式，并且缓冲区会在重连时重新分配。使用 `start_reads_borrowed()` 时，负载直接落在 `acquire_rx_buffer` 返回的缓冲区中；返回空缓冲区会施加背压，直到调用 `resume_reads()`。token 可用于标识每个借用 span 的归属。provider 与完成回调在 Astrial 的 libusb 事件线程上执行。读取的 provider/回调分发与单次在途写入闸门在传输热路径上不加锁；生命周期操作仍会对 `start_reads()`/`start_reads_borrowed()`、`stop_reads()`、重连和关闭进行同步。

对于 borrowed 读取，`UsbReadBufferPool` 会分配一组可复用的页对齐缓冲区并跟踪空闲状态，保证 provider 每次拿到的都是互不重叠且仍然存活的缓冲区：

```cpp
auto pool = UsbReadBufferPool::create(64 * 1024, 8).value();

device.start_reads_borrowed(
    [&] { return pool.acquire(); },
    [&](const std::error_code& error, UsbBorrowedBuffer buffer,
        std::size_t length) {
        consume(buffer, length, error);
        pool.release(buffer); // 归还到空闲链表
    });
```

当所有缓冲区都被占用时，`acquire()` 会返回空缓冲区，自动施加读取循环的背压。

`async_write_borrowed()` 同样会保持调用方的内存在完成前原地不动。当一个非空 OUT 传输恰好是端点包大小的整数倍时，Astrial 会显式发送一个零长度包，即使在 libusb 自动 ZLP 标志不可用的平台上也是如此。

## 测试

在 Linux 上，集成测试会创建一个原始伪终端（pseudo-terminal），无需物理串口硬件即可同时测试自有缓冲区与借用式 I/O：

```bash
cmake -S . -B build -DASTRIAL_BUILD_TESTS=ON -DASTRIAL_IO_URING=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

PTY 测试覆盖传输行为和控制能力不支持时的明确错误。Linux 静态构建还会运行链接边界的驱动响应
测试，覆盖控制映射、错误、定时器、回调与重连恢复；这些软件测试不验证电气行为。

可选的 `tests/serial_hil.cpp` 验证物理 RS-232 回环：断开其他设备，将 DB9 **2-3**、
**7-8**、**4-1-6-9** 分别连接。它检查二进制收发，以及 DTR/RTS 的全部输出组合与
CTS/DSR/DCD/RI 的实际对应状态。

```bash
cmake -S . -B build -DASTRIAL_BUILD_TESTS=ON -DASTRIAL_BUILD_HIL_TESTS=ON
cmake --build build
ASTRIAL_HIL_PORT=/dev/ttyUSB0 ASTRIAL_HIL_FIXTURE=rs232-loopback \
  ctest --test-dir build -R '^astrial_serial_hil$' --output-on-failure
```

缺少任一环境变量时会跳过（退出码 77）。MCU 或其他 RS-232 对端连接着时，不要运行
回环模式。独立帧格式检查、实际硬件流控暂停、BREAK 波形与检测、错误注入及物理重连
仍需独立对端或仪器；工具会明确列为未测试，不将回环通过当作完整硬件验证。RS-485
另行处理。

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
