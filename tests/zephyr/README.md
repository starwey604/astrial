# Astrial Zephyr firmware tests

板载固件，用于 Astrial USB HS bulk 的硬件在环（HIL）测试。宿主机侧的
HIL 程序见 `tests/usb_lifecycle_hil.cpp`（将来还有 `usb_perf_hil`）。

**这些应用不参与 CTest，也不进 CI**——它们需要本机的 west workspace 和
Zephyr SDK，由 `tests/CMakeLists.txt` 显式列举源文件的方式隔离，宿主机构建
完全看不到这个目录。

## 目录

| 应用 | 里程碑 | 作用 |
|---|---|---|
| `usb_enum/` | M1+M2 | 枚举检查固件：VID/PID `2fe3:574c`、interface 0、bulk `0x01`/`0x81`，同时就是后续 HIL 测试的目标设备 |

## 前置条件

- west workspace 位于 `~/zephyrproject`（Zephyr main，本次验证过的 commit：`933fe1291d2`）
- Zephyr SDK 1.0.1 已注册（`~/.cmake/packages/Zephyr-sdk`）
- west 在 workspace 的 venv 里：`~/zephyrproject/.venv/bin/west`
- Linux 主机（枚举检查用 `lsusb`/`dmesg`）
- FRDM-MCXN947 板，USB-C 线连到**板上目标 USB HS 口**（不是 MCU-Link 调试口）

  > 注意板上有两个 USB-C 口：MCU-Link 调试口（供电 + 烧录 + 串口）和目标
  > USB HS 口。插错口 `lsusb` 里看不到 `2fe3:574c`。以板上丝印和
  > [NXP 用户手册](https://www.nxp.com/design/development-boards/freedom-development-boards/mcx-n-series-mcx-n94x-board:FRDM-MCXN947)
  > 为准（首跑时确认一次，把结论记在这里）。

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
[...] <inf> astrial_usb_enum: USBD message: VBUS ready        (或 "USB device support enabled")
[...] <inf> astrial_usb_enum: USBD message: Bus reset
[...] <inf> astrial_usb_enum: USBD message: New device configuration
[...] <inf> astrial_usb_enum: Astrial USB enum check ready: 2fe3:574c
```

主机侧检查：

```bash
# 1. 设备出现，VID/PID 正确
lsusb -d 2fe3:574c
# 期望: Bus ... Device ... ID 2fe3:574c

# 2. 走的是 High Speed（关键——480Mb/s 才有意义）
dmesg --ctime | tail -n 15
# 期望: "new high-speed USB device ... using ehci-pci/..." 之类，
#       若出现 "full speed" 即 HS 没起来，回头查 PHY/接线

# 3. 端点布局符合 astrial::usb 的默认期望
sudo lsusb -v -d 2fe3:574c 2>/dev/null | \
    grep -E 'bInterfaceClass|bEndpointAddress|wMaxPacketSize|bmAttributes'
# 期望: interface 0 = Vendor Specific (0xff)
#       EP 0x01 Bulk OUT, EP 0x81 Bulk IN
#       wMaxPacketSize 0x0200 (512 = HS bulk)
```

三项全中 → M2 通过。随后可直接拿现有 HIL 工具对真机跑：

```bash
cmake -B build -DASTRIAL_BUILD_USB=ON -DASTRIAL_BUILD_USB_HIL=ON
cmake --build build --target astrial_usb_lifecycle_hil
./build/tests/astrial_usb_lifecycle_hil            # 默认 VID/PID 即匹配
```

## 环境变量（宿主侧 HIL 工具）

暂无——`usb_lifecycle_hil` 默认 VID/PID `2fe3:574c` 与固件一致，直接跑。

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
- 记录验证过的 Zephyr commit（当前 `933fe1291d2`），workspace 滚动更新后
  重新跑一遍枚举检查。
