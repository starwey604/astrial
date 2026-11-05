# RS-232 fixture v1 — FRDM-MCXN947

独立 Zephyr 应用，用现有 USB HS 测试板充当 Astrial 的 RS-232 对端。
**本版只完成编译和软件测试；等待转接板换装 MAX3237EEAI+ 后再烧录验证。**
`../usb_enum`、`../usb_perf` 保持独立，不修改其固件、构建目录或已有烧录内容。
将来烧录本应用会替换板上当前程序；需要 USB HS 测试时重新烧录对应应用。

## 两条独立通道

- **控制**：电脑 → J17 MCU-Link VCOM → LPUART4（P1_8 RX / P1_9 TX），
  固定 115200 8N1，无流控。没有日志、提示符或启动横幅。控制端也可用 Astrial 打开。
- **数据**：Astrial → PL2303 → DB9 → MAX3237 → LPUART6 / GPIO。
  J11 USB HS 不参与本应用，无需连接。

控制通道独立于被测数据格式及流控；即使数据侧被 XOFF 或 RTS 门控暂停，
仍能查询状态、改握手输出或 STOP。控制端应持续读取回复，不要无限流水发送命令。

## 接线：仅接 MAX3237 的 3.3 V 逻辑侧

先断电。**不要用已误装的 MAX3243 上电联调，不要把 DB9 的正负 RS-232 电压
直接接进 NXP GPIO。** 裸 FRDM-MCXN947 不装 mikroBUS、摄像头或其他占用下列引脚的模块。

| 转接板 H1 | FRDM-MCXN947 | MCU 方向 | 电脑（DTE）侧含义 |
|---|---|---|---|
| 1 `MCU_TXD` | J6.4 / P3_21 / UART6 TX | 输出 | PC RXD |
| 3 `MCU_RXD` | J6.6 / P3_20 / UART6 RX | 输入 | PC TXD |
| 5 `MCU_RTS` | J6.3 / P3_23 | 输出 | PC CTS |
| 7 `MCU_CTS` | J6.5 / P3_22 | 输入 | PC RTS |
| 2 `MCU_DSR` | J1.6 / P0_29 | 输出 | PC DSR |
| 4 `MCU_DCD` | J1.8 / P1_23 | 输出 | PC DCD |
| 6 `MCU_RI` | J1.10 / P0_30 | 输出 | PC RI |
| 8 `MCU_DTR` | J1.16 / P0_31 | 输入 | PC DTR |
| 9 GND | J6.8 GND | 共地 | 信号参考 |
| 10 3V3 | 两板分别供电时不接 | — | 避免电源互灌 |

J6 的 SPI 接口在此复用为 Flexcomm6 UART，overlay 禁用同一 Flexcomm 的 SPI/I2C
及冲突的 SAI1。物理排针位置以
[NXP 板卡手册](https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/MCX/4254/1/UM12018%20%283%29.pdf)
及板上标记为准；上述接线仍需首轮实物核对。

六路握手信号经过 MAX3237 反相，MCU 逻辑侧低电平表示 asserted。
设备树统一 `GPIO_ACTIVE_LOW`，协议中的 **1 一律表示电脑端信号有效**。
UART TX/RX 不额外反相。应用初始化后四路握手输出默认无效（GPIO 高电平），
RTS/DTR 输入上拉。GET_LINES 的四路输出值是最后成功设置的逻辑值，不是 DB9 电压测量。

## 编译与软件测试（不烧录）

本机验证环境：2026-11-05，Zephyr **`b37b2126db4`（4.4.99）**、
Zephyr SDK **1.0.1**、目标 `frdm_mcxn947/mcxn947/cpu0`。
板卡寄存器操作集中在 `src/main.c`，绑定此芯片的 LPUART FIFO/TC；
协议与任务状态机 `src/fixture.c` 不依赖 Zephyr。

```bash
cd ~/zephyrproject
.venv/bin/west build -b frdm_mcxn947/mcxn947/cpu0 \
    -d build/astrial-rs232-fixture \
    ~/codings/astrial/tests/zephyr/rs232_fixture
```

产物：`~/zephyrproject/build/astrial-rs232-fixture/zephyr/zephyr.elf`、`zephyr.bin`。
此阶段不执行 `west flash`，不打开 PL2303 或 MCU-Link 串口。

纯软件测试独立于宿主库 CMake 和 Zephyr workspace，直接编译固件使用的同一份状态机：

```bash
cd ~/codings/astrial
cmake -S tests/zephyr/rs232_fixture/tests -B /tmp/astrial-rs232-tests \
    -DCMAKE_BUILD_TYPE=Debug -DFIXTURE_SANITIZERS=ON
cmake --build /tmp/astrial-rs232-tests
ctest --test-dir /tmp/astrial-rs232-tests --output-on-failure
```

软件测试覆盖严格解析和溢出重同步、全部允许的 UART 格式组合、配置失败回滚及故障恢复、
16 种输出组合、输入读回、分批发送、硬件发送完成门槛、二进制回显、错字节定位、
XON/XOFF、RTS 门控、超时、STOP、UART 错误及 RX 溢出失败、控制回复背压、
最大计数格式化和 20,000 轮确定性随机解析输入。ASan/UBSan 检查通过。
模拟后端验证逻辑，不执行 Zephyr IRQ、GPIO 或硬件寄存器；这些部分交叉编译通过，
实际电平、脚位、时序和 PL2303 的信号覆盖由后续硬件验证验收。

## 控制协议 V1

ASCII 行，LF 结束，可接受 CRLF；参数用空格分隔，命令区分大小写。
每行最多 511 字节（不含 LF）；非法字符、超长行或控制 RX 溢出后丢弃到下一个 LF。
溢出可能丢掉一个完整命令，必须重新同步并查询 STATUS，不能假定动作执行过。

```
V1 <seq> <COMMAND> [arguments...]
V1 <seq> OK [fields...]
V1 <seq> ERR <reason>
V1 0 DONE job=<原始seq> kind=TX|RX|ECHO result=... count=... mismatches=... first_bad=...
```

`seq` 为 1..4294967295 十进制整数，电脑端自行分配、不要复用未完成任务的序号。
0 留给异步 DONE 和无法关联命令的解析错误。序号只用于关联，**不提供重试去重**。
逐条等待 ACK；一个数据任务的 ACK 仅表示已启动，必须等待 DONE。
DONE 也可能出现在后续命令的回复之前。STATUS 保留最近任务结果，便于恢复查询。

| 命令 | 参数 | 用途 |
|---|---|---|
| `CAPS` | 无 | 实际支持项及协议版本 |
| `CONFIG` | `baud bits parity stops flow` | 配置数据 UART，控制 UART 不变 |
| `SET_LINES` | `cts dsr dcd ri` | 四个值分别为 0 或 1；允许任务运行中改变 |
| `GET_LINES` | 无 | 四路输出逻辑状态，以及采样得到的 PC RTS/DTR |
| `TX_PATTERN` | `count seed timeout_ms` | 向电脑发送确定性数据 |
| `RX_VERIFY` | `count seed timeout_ms` | 校验电脑发来的数据 |
| `ECHO` | `count timeout_ms` | 收到 count 个有效载荷字节并原样回显 |
| `STATUS` | 无 | 当前配置、任务进度、累计计数、故障和最近结果 |
| `STOP` | 无 | 中止/清空数据通道，恢复当前配置，四路输出置无效 |

配置范围：
- 波特率：1200、2400、4800、9600、19200、38400、57600、115200、230400。
- 数据位 7/8，校验 `N`/`O`/`E`，停止位 1/2。默认 115200 8N1。
- `flow=NONE|XONXOFF|RTS_GATE`，默认 NONE。
- `count=1..16777216`，`seed=0..255`，`timeout_ms=1..3600000`，均为十进制。
  超时为从接收命令开始的总时限，暂停期间也计时，不自动按波特率延长。

CONFIG 和新任务要求 IDLE 且硬件发送完全结束，否则 ERR BUSY。
非法配置先拒绝，不调用驱动；驱动配置失败会尝试恢复旧配置。
恢复失败或 GPIO 设置失败进入 fault，STATUS 仍可读；STOP 成功后才恢复正常任务。
启动新任务清空旧数据 RX，电脑必须先停止发送、发命令、等 OK 后再发送数据。
CONFIG/STOP 前也要停止对端发送；无法用软件区分跨越 UART 重配置的半个物理帧。

### 数据和结果的精确定义

普通模式：第 i 个有效载荷字节为 `(seed + i) % (1 << bits)`，i 从 0 开始。
XONXOFF 模式：alphabet 为 `2^bits - 2`，先取 `(seed + i) % alphabet`，
若值 ≥ 0x11 则加 1，再若值 ≥ 0x13 则加 1。这样数据不含 XON/XOFF。

RX_VERIFY 比较全部 count 个字节，统计不匹配数和首个不匹配偏移；
`first_bad=4294967295` 表示尚无不匹配。不会靠收到 NUL 判断 BREAK。
ECHO 在 XONXOFF 模式同样剔除收到的流控字符；要完整二进制回显请选择 NONE。

- TX 的 `progress/count` 是已进入发送队列的字节，ECHO 是已接收并排队回显的字节。
  **只有 result=OK 的 TX/ECHO DONE 才保证队列与 UART 移位器全部发送完成。**
- RX 的 DONE 只覆盖请求的 count 字节；多余字节计入 unexpected。
  应同时检查电脑收到的长度、固件 unexpected 增量，不能用 DONE 证明链路上没有后续多余字节。
- 结果包括 `OK`、`MISMATCH`、`TIMEOUT`、`CANCELLED`、`UART_ERROR`、
  `RX_OVERFLOW`、`ECHO_OVERFLOW`、`IO_ERROR`。错误/超时主动清空数据队列，不无限等待 drain。
  STOP/失败中止可能截断当前物理帧，下一轮先停止对端、清理主机缓冲再重新开始。
- `rx` 是交给状态机处理的字节数，包含流控字符和非任务数据，不含重配置/中止主动丢弃的缓冲，`tx_queued` 是累计排队字节（含后来被中止丢弃的字节），
  `unexpected` 不包含被消费的 XON/XOFF，`rx_dropped` 是软件 RX ring 丢字节数。
  `uart_errors` 是观察到硬件错误标志的次数，不是准确的损坏字节数，不等于电脑驱动错误计数。
  `control_lost` 合计控制 RX ring 拒收字节数及控制 UART 错误观察次数；
  检测溢出后为重同步而清掉的旧缓冲不另计。
  累计计数不因 STOP 清零，重启清零；测量应使用前后差值。
- 输入约每个主循环（通常 1 ms）采样；GET_LINES 不是边沿捕获。
  输出逐 GPIO 设置，不保证多路同时切换。

### 流控如何测试

`XONXOFF` 处理 **电脑发来的** 0x13/0x11，暂停/恢复板卡 TX。
它不根据本板 RX 缓冲水位自动发送 XOFF，缓冲不足会明确失败。
要测试电脑发送端响应 XOFF，可在 NONE 模式下用 `TX_PATTERN 1 19 1000`
发送一个 0x13，随后 `TX_PATTERN 1 17 1000` 发送 0x11（两次均等 DONE）。

`RTS_GATE` 按采样的 **PC RTS** 门控板卡 TX，板卡 PC CTS 输出仍由 SET_LINES 独立控制。
这是软件门控，不是 MCU 原生自动 RTS/CTS；暂停有采样延迟，FIFO/移位器中已装入的字节
仍可发出。若 PC 启用硬件流控发送，先 SET_LINES 让 CTS=1，否则 PC 可能一直等待。

首版不实现 BREAK 生成/专门检测、Mark/Space、5/6 数据位、1.5 停止位、
RS-485、物理 USB 拔插模拟及自动重连测试。CAPS 显式报告
`break=0 mark_space=0 native_rtscts=0`。上述功能不能仅凭本固件验收。

## 换芯片后的首轮硬件验收顺序

1. 断电核对 MAX3237 型号/方向、接线和共地；分别供电，不接 H1.10。
   先确认转接板电源及 RS-232 电平，再连接 NXP 逻辑侧。
2. 经 J17 烧录本应用后，控制端发 `V1 1 CAPS`，确认协议可用；
   `V1 2 GET_LINES` 核对默认输出，再逐路 SET_LINES，检查 PL2303 看到的 CTS/DSR/DCD/RI。
   在电脑端分别切换 RTS/DTR，用 GET_LINES 核对，确定适配器究竟引出了哪些信号。
3. 双方 115200 8N1、NONE，发送 `V1 3 RX_VERIFY 256 0 5000`，等 OK 后
   Astrial 写入 0..255；等待 DONE result=OK，再反向 `V1 4 TX_PATTERN 256 0 5000`
   校验电脑收到的全部 256 字节，随后测试有限长度 ECHO。
4. 扩展到 7/8 位、N/O/E、1/2 停止位及允许波特率，双方配置一致；
   分别验证 CTS 暂停电脑发送、PC RTS 门控板卡发送、双向 XON/XOFF。
   降低波特率便于观察暂停后的尾部字节。
5. 注入超时、数据错误、STOP，确认下一轮恢复；再接入 Astrial HIL 自动化。
   此应用不替代现有 `serial_hil` scaffold，也尚未让它自动验收真实硬件。

第 1–5 步均尚未执行，等待 MAX3237 换装后进行。
