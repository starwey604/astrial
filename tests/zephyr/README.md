# Astrial Zephyr firmware tests

独立板载固件，用于 Astrial USB HS bulk 和 RS-232 的硬件在环（HIL）测试。USB 宿主机侧的
HIL 程序见 `tests/usb_lifecycle_hil.cpp`（将来还有 `usb_perf_hil`）。

**这些固件应用不参与宿主库的 CTest，也不进 CI**——它们需要本机的 west workspace 和
Zephyr SDK，由 `tests/CMakeLists.txt` 显式列举源文件的方式隔离，宿主机构建
完全看不到这个目录。`rs232_fixture/tests` 另有独立的纯软件 CTest，
不依赖 Zephyr 或硬件。

## 目录

| 应用 | 里程碑 | 作用 |
|---|---|---|
| `usb_enum/` | M1+M2 | 枚举检查固件：VID/PID `2fe3:574c`、interface 0、bulk `0x01`/`0x81`，同时就是后续 HIL 测试的目标设备 |
| `usb_perf/` | M3 | 独立 bulk echo 固件：VID/PID `2fe3:574d`、interface 0、bulk `0x01`/`0x81`，用于延迟和大包性能测试 |
| [`rs232_fixture/`](rs232_fixture/README.md) | RS-232 v1 | FRDM-MCXN947：J17 控制协议、UART6 数据收发、六路握手 GPIO；仅完成交叉编译与软件测试，尚未烧录 |

RS-232 的接线、构建和验收见其独立 README；以下 USB 章节保留原有实测记录。

## 前置条件

- west workspace 位于 `~/zephyrproject`（Zephyr main，本次验证过的 commit：`933fe1291d2`）
- Zephyr SDK 1.0.1 已注册（`~/.cmake/packages/Zephyr-sdk`）
- west 在 workspace 的 venv 里：`~/zephyrproject/.venv/bin/west`
- Linux 主机（枚举检查用 `lsusb`）
- FRDM-MCXN947 板，两根可传数据的 USB-C 线：**J17** 接 MCU-Link（供电、
  烧录和串口），**J11** 接目标 USB HS 口（设备枚举）

  > 只连接 J17 时固件可以烧录和打印串口日志，但 `lsusb` 不会看到
  > `2fe3:574c`。J11 的用途见 [NXP 板卡手册](https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/MCX/4254/1/UM12018%20%283%29.pdf)。

## 构建（M1）

```bash
cd ~/zephyrproject
.venv/bin/west build -p -b frdm_mcxn947/mcxn947/cpu0 \
    -d build/astrial-usb-enum \
    ~/codings/astrial/tests/zephyr/usb_enum
```

构建产物：`~/zephyrproject/build/astrial-usb-enum/zephyr/{zephyr.bin,zephyr.elf}`。

## 烧录（M2）

板载 MCU-Link，默认 runner 是 LinkServer：

```bash
cd ~/zephyrproject
.venv/bin/west flash -d build/astrial-usb-enum
```

LinkServer 不可用时依次尝试：`west flash -r jlink`（需 J-Link 固件的
MCU-Link）、`west flash -r pyocd`（需 `pyocd pack install mcxn947`），或
按板手册进 ISP（SW3/SW1）后用 `blhost` 烧 `zephyr.bin`。

串口日志：115200 8N1，接 MCU-Link 口的虚拟串口（`/dev/ttyACM*`）。

## 枚举检查（M2 验收）

固件启动后串口应出现：

```
[...] <inf> astrial_usb_enum: UDC capability: high-speed
[...] <inf> astrial_usb_enum: USB device support enabled
[...] <inf> astrial_usb_enum: Astrial USB enum check ready: 2fe3:574c
[...] <inf> astrial_usb_enum: USBD message: Bus reset
[...] <inf> astrial_usb_enum: USBD message: New device configuration
```

主机侧检查：

```bash
# 1. 设备出现，VID/PID 正确
lsusb -d 2fe3:574c
# 期望: Bus ... Device ... ID 2fe3:574c

# 2. 走的是 High Speed（关键——480Mb/s 才有意义）
lsusb -v -d 2fe3:574c 2>/dev/null | grep 'Negotiated speed'
# 期望: Negotiated speed: High Speed (480Mbps)

# 3. 端点布局符合 astrial::usb 的默认期望
lsusb -v -d 2fe3:574c 2>/dev/null | \
    grep -E 'bInterfaceClass|bEndpointAddress|wMaxPacketSize|bmAttributes'
# 期望: Zephyr loopback 类有三个 Vendor Specific interface（带 IAD），
#       astrial::usb 使用 interface 0
#       EP 0x01 Bulk OUT, EP 0x81 Bulk IN
#       wMaxPacketSize 0x0200 (512 = HS bulk)
```

三项全中 → M2 通过。

2026-09-24 在 FRDM-MCXN947、Zephyr `933fe1291d2`、LinkServer
`v26.9.130` 上实测三项通过：`lsusb` 显示 `2fe3:574c`，总线速度
`480 Mbps`，interface 0 为 Vendor Specific，bulk OUT `0x01` 与 IN
`0x81` 的 `wMaxPacketSize` 均为 `512`。J11 在启动前已连接时，EHCI
驱动没有报告 `VBUS_READY`；测试固件在 `usbd_init()` 后主动调用
`usbd_enable()`，串口随即出现 Bus reset 和 New device configuration。

随后可直接拿现有 HIL 工具对真机跑：

```bash
cmake -B build -DASTRIAL_BUILD_USB=ON -DASTRIAL_BUILD_USB_HIL=ON
cmake --build build --target astrial_usb_lifecycle_hil
./build/tests/astrial_usb_lifecycle_hil            # 默认 VID/PID 即匹配
```

## 环境变量（宿主侧 HIL 工具）

暂无——`usb_lifecycle_hil` 默认 VID/PID `2fe3:574c` 与固件一致，直接跑。

## Bulk 延迟与大包性能（M3）

`usb_enum` 所用的 Zephyr 内置 loopback 会持续向 IN 端点发送 1 KiB
缓冲区；它不能把一轮 OUT 与一轮 IN 唯一对应。性能测试改用
`usb_perf`：固件把每个 OUT 数据块原样送回 IN，逐块完成后再接收下一块。
固件内部块大小为 4096 字节，宿主的一次写入可跨多个块。宿主程序逐字节
校验回显，任何错误、数据错位或超时都会以非零退出码结束。

```bash
cd ~/zephyrproject
.venv/bin/west build -p -b frdm_mcxn947/mcxn947/cpu0 \
    -d build/astrial-usb-perf \
    ~/codings/astrial/tests/zephyr/usb_perf
.venv/bin/west flash -d build/astrial-usb-perf

cd ~/codings/astrial
cmake -B build -DASTRIAL_BUILD_USB=ON -DASTRIAL_BUILD_USB_HIL=ON \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build --target astrial_usb_latency_hil astrial_usb_throughput_hil \
    astrial_usb_latency_raw_hil
./build/tests/astrial_usb_latency_hil --size 64 --rounds 300 --warmup 20
./build/tests/astrial_usb_throughput_hil --size 65536 --rounds 64 --warmup 4
./build/tests/astrial_usb_throughput_hil --size 1048576 --rounds 16 --warmup 2
# 时间戳剖析：同一固件上的 Astrial 与原生 libusb 对照
./build/tests/astrial_usb_latency_hil --profile --size 64 --rounds 2000 --warmup 100
./build/tests/astrial_usb_latency_raw_hil --size 64 --rounds 2000 --warmup 100
# 固件的 4096 B OUT 缓冲区可以在满缓冲后回显，无需 ZLP；需要时显式选用
./build/tests/astrial_usb_latency_hil --size 4096 --rounds 500 --warmup 25
./build/tests/astrial_usb_latency_hil --size 4096 --rounds 500 --warmup 25 --zlp
```

延迟是调用 `async_write_borrowed()` 前到收到完整回显的往返时间
（RTT）；输出最小值、p50、p95、p99、最大值，单位微秒。
`echo_MiB_per_s` 是逐轮写入并校验回显后的有效负载字节数除以总耗时；
它是全双工往返测试的吞吐，不是单向 USB 链路上限。每轮只允许一笔写入，
当前固件一次只处理一个 4096 字节块，因此结果还包含宿主和固件的调度开销。
上述工具只在 `ASTRIAL_BUILD_USB_HIL=ON` 时构建，均不加入 CTest。
`--profile` 使用 32 字节诊断头，固件写入 OUT 回调与 IN 提交前的本地
周期计数及频率；宿主只计算两个设备时间戳之差，不把两个设备的时钟
直接相减。原生 libusb 工具用相同的 64 字节请求和预先提交的 IN 传输，
作为 Astrial 路径的近似基线。

### 2026-09-24 实测

环境：FRDM-MCXN947、Zephyr `933fe1291d2`、Zephyr SDK 1.0.1、
Linux xHCI（经 USB 2.0 hub）、Astrial Release 构建。设备以
`2fe3:574d`、High Speed（480 Mbps）枚举。每组重复 3 次；表中列出
三次结果的范围，预热轮次不计入统计。所有轮次的回显字节均通过校验。
此表是首轮固件与自动补 ZLP 的基准记录；下文另列修改后的复查。

| 测试 | 每次运行 | RTT p50 | RTT p99 | 有效吞吐 |
|---|---:|---:|---:|---:|
| 64 B 延迟 | 1000 轮 | 76.70–77.14 µs | 84.90–92.38 µs | 0.76–0.77 MiB/s |
| 64 KiB 大包 | 128 轮，8 MiB/次 | 4.08–4.17 ms | 4.15–4.87 ms | 14.40–15.14 MiB/s |
| 1 MiB 大包 | 16 轮，16 MiB/次 | 64.23–68.85 ms | 64.38–71.92 ms | 14.28–15.45 MiB/s |

这是逐块回显且每次只有一笔写入的结果，不能作为 USB HS 单向峰值
带宽。若宿主程序报告 `permission denied`，需给当前 `2fe3:574d` 对应
的 `/dev/bus/usb/BBB/DDD` 节点授予读写权限；节点号在每次重新枚举后
可能改变。

### 延迟归因与 ZLP 复查

同一台主机与固件上，64 B、每组 2000 轮、交替运行 3 组：原生 libusb
RTT p50 为 71.27–71.76 µs，Astrial 为 74.85–74.86 µs；固件 OUT
回调到 IN 提交前的 p50 为 0.92 µs。写完成回调 p50 分别为
31.37–32.15 µs 和 35.67–36.19 µs。两种宿主程序的读请求重挂与
线程同步实现不同，因此 3–4 µs 差距是近似对照，不是严格隔离出的
Astrial 净开销。尝试在 Astrial 读完成回调内立即重挂时，RTT p50 为
74.25–74.81 µs，和原有范围重叠；未保留这项并发路径改动。

4096 B 请求在本固件上不补 ZLP 也能正确回显。交替运行 3 组、每组
500 轮：不补 ZLP 的 RTT p50 为 236.20–237.09 µs、吞吐
15.45–15.56 MiB/s；补 ZLP 的 p50 为 250.18–253.03 µs、吞吐
13.07–13.22 MiB/s。512 B 请求在固件的 4096 B
OUT 缓冲区上则需要短包终止，显式 `--zlp` 测得 p50 116.97 µs。
这表明 ZLP 由设备接收策略和上层消息边界决定，Astrial 默认不添加，
调用方可通过 `UsbWriteOptions` 逐笔选择。[libusb 对 ZLP 的说明](https://libusb.sourceforge.io/api-1.0/group__libusb__asyncio.html)
也将其描述为设备协议需要短包结束逻辑请求时使用的选项。
改为默认不补 ZLP 并保留原有读重挂路径后，最终复查得到 64 B RTT
p50 74.64 µs、1 MiB 回显吞吐 15.35 MiB/s；`512 B --zlp`、
USB lifecycle HIL（2 轮）与常规 CTest 均通过。

## 故障排查

| 症状 | 原因/处理 |
|---|---|
| `lsusb` 无 `2fe3:574c` | 插错 USB-C 口；或看串口日志是否根本没启动 |
| 串口无日志 | 换 MCU-Link 口、115200 8N1；确认烧录成功（重烧后按复位） |
| 枚举为 full speed | HS PHY 没起——确认板 dtsi 的 `usbphy1` tx 校准参数、线材/口 |
| `west build` 找不到 board | 先 `west update` 确认 workspace 完整；board 名必须是 `frdm_mcxn947/mcxn947/cpu0` |
| 反复重枚举 | USB 线供电不稳或接触不良，换线 |

## 约定（给后来的 agent）

- 新固件 app 放这里，一目录一 app（`CMakeLists.txt` + `prj.conf` + `src/`），
  不要碰宿主机的 `tests/CMakeLists.txt`。
- 不要引入 `samples/subsys/usb/common` 的样例基建；自己的 Kconfig/描述符
  自己写（本 repo 的约定，见本目录 README 顶部）。
- VID/PID、端点布局以 `include/astrial/Usb.hpp` 的默认值为准；改任何一边
  都要同步 `usb_lifecycle_hil` 的默认值。
- 每个应用记录各自验证过的 Zephyr commit（上述 USB 实测为 `933fe1291d2`），workspace 滚动更新后
  重新跑一遍枚举检查。
