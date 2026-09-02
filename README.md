# 5V SigGen —— STM32G474 多脉冲 / 双脉冲发波板

基于 **STM32G474CET6**（Arm Cortex-M4 @ 170MHz）的高精度脉冲 / 功率驱动信号发生器固件。板载 **HRTIM1 高精度定时器**（最高 170MHz × 32 = 5.44GHz 时间分辨率）输出 6 路 SMA 波形，配套 128×128 OLED 菜单、旋转编码器 / 五向摇杆人机交互，以及 USB-TTL 上位机（OLED 镜像 + SCPI 远程控制）。

> 硬件接线与电气特性以根目录 [`HARDWARE.md`](HARDWARE.md) 为**唯一基准**。

---

## 1. 核心功能

- **7 种发波模式**：N 脉冲（短/长）、双脉冲、PWM（短/长）、互补 PWM（高精度 / 超长）。
- **高精度发波**：HRTIM1 自动分频档（MUL32 ~ DIV4）覆盖 0.01µs ~ 1500µs 脉宽 / 周期，最细约 0.18ns 分辨率。
- **互补死区防直通**：互补 PWM 由 HRTIM 死区发生器硬件生成上升/下降沿死区（0 ~ 12000ns，约 12µs）；超长互补由 TIM5 软件死区实现（1 ~ 5000ms）。
- **SYNC OUT + 帧标记**：Y7 输出与首脉冲 ns 级对齐的 200ns 同步脉冲，Y8 输出猝发帧标记（示波器触发用）。
- **Burst 猝发重复**：TIM3 作 PRF 时基，1Hz ~ 100kHz 周期猝发复读（0 = 单次触发）。
- **硬件故障封锁**：`PA15 = HRTIM1_FLT2` 低有效输入，触发后 A/B/D 三组输出 ns 级强制无效，并软件网关瞬间切断 12V 负载。
- **12V_OUT 手动控制**：负载开关受 `PA12` 控制，掉电 / 故障 / 软件关断均在微秒级切断。
- **Preset 参数存储**：全部模式参数写入片内 Flash 末页（CRC 校验），断电保存 / 一键调出。
- **SCPI 远程控制 + OLED 镜像**：USB-TTL 串口（USART3, 460800 8N1）实现屏幕 1:1 镜像推流、SCPI 文本命令、虚拟按键注入；状态变化主动推送，上位机 0 延迟同步。
- **UI 交互动效**：数值弹窗（SpinWin/ValWin）触界回弹、列表光标果冻形变（基于 WouoUI 框架增强）。

---

## 2. 硬件平台概览

| 项目 | 说明 |
|:---|:---|
| 主控 | STM32G474CET6，LQFP48，512KB Flash / 96KB SRAM（SRAM1+SRAM2，链接脚本映射；另有 32KB CCM SRAM） |
| 系统时钟 | HSE 24MHz → PLL 170MHz（PLLM=6, PLLN=85, PLLP=2, Voltage Scale1 Boost） |
| 高精定时器 | HRTIM1（170MHz，`PrescalerRatio` 最高 MUL32 → 5.44GHz） |
| 波形输出 | Y1~Y6 为 CH1~CH6（SMA），经 74LVCH8T245 电平转换；Y7=SYNC OUT，Y8=帧标记 |
| 显示 | 0.96/1.3 寸 128×128 OLED（SSD1315 兼容，SPI1 + DMA） |
| 人机交互 | EC12D 旋转编码器（TIM2 编码器模式）+ 五向摇杆（MAX6818 硬件消抖） |
| 电源 | USB-C PD(IP2721) / 2EDG 端子 双路 12V → LTC4421 理想二极管 → 5V/3.3V；12V_OUT 经 TPS22810 输出 |

---

## 3. 发波模式与参数

通道映射（严格对齐 `HARDWARE.md §5.1`，Timer C 已剥离发波）：

| 逻辑通道 | HRTIM 通道 | MCU 引脚 | SMA |
|:---|:---|:---|:---|
| CH1 | HRTIM1_CHB2 | PA11 | Y1 |
| CH2 | HRTIM1_CHB1 | PA10 | Y2 |
| CH3 | HRTIM1_CHA2 | PA9  | Y3 |
| CH4 | HRTIM1_CHA1 | PA8  | Y4 |
| CH5 | HRTIM1_CHD2 | PB15 | Y5 |
| CH6 | HRTIM1_CHD1 | PB14 | Y6 |
| SYNC OUT | HRTIM1_CHC2 | PB13 | Y7 |
| 帧标记 | GPIO (软件) | PB12 | Y8 |

发波模式（主菜单 7 项 + Setting + About）：

| 模式 | 驱动硬件 | 可调参数 |
|:---|:---|:---|
| Multi Pulse（N 脉冲） | HRTIM 可重触发单次 | 脉宽(µs)、脉冲数(1~100)、间隔(µs)、Burst PRF(Hz) |
| Multi Pulse Long（N 长脉冲） | TIM5 + GPIO 软件翻转 | 脉宽(s)、脉冲数、间隔(s) |
| Double Pulse（双脉冲） | HRTIM 单次 | 第 1 脉宽(µs)、间隔(µs)、第 2 脉宽(µs) |
| PWM | HRTIM 连续 | 周期(µs)、占空比(%) |
| PWM Long | TIM5 + GPIO | 周期(s)、占空比(%) |
| Comp PWM（互补 PWM） | HRTIM 连续 + 硬件死区 | 通道对、周期(µs)、占空比(%)、DT 上升沿(ns)、DT 下降沿(ns) |
| Comp PWM Long（超长互补） | TIM5 + 软件死区 | 通道对、周期(s)、占空比(%)、死区(ms) |

互补通道对：`CH1&CH2`(Timer B)、`CH3&CH4`(Timer A)、`CH5&CH6`(Timer D)，主路 = Tx1，互补路 = Tx2。

---

## 4. 工程架构

```
G474-Test/
├── Core/
│   ├── Inc/                 # 头文件（含 WouoUI 框架、Pulse.h、uart_comm.h 等）
│   └── Src/
│       ├── main.c           # 主循环：UI、12V 控制、通信、Fault 收尾
│       ├── Pulse.c/.h       # ★ 发波核心：7 模式 + SYNC + Burst PRF + Fault
│       ├── uart_comm.c/.h   # ★ PC 通信协议层（镜像推流 + SCPI + 虚拟按键）
│       ├── usart.c/.h       # USART3 底层（460800 8N1, TX=DMA, RX=RXNE 中断）
│       ├── Preset.c/.h      # Flash 参数存储/调出（0x0807F800, CRC 校验）
│       ├── WouoUI_user.c    # UI 数据模型（各模式 Option[] 数组 + 页面回调）
│       ├── WouoUI*.c        # WouoUI 框架（菜单/动画/字体/绘图/窗口）
│       ├── OLED*.c          # SSD1315 OLED 底层驱动 + 字体
│       ├── Key.c/.h         # 按键扫描（MAX6818 消抖 + 编码器）
│       └── stm32g4xx_it.c   # 中断服务（EXTI1/TIM5/TIM3/USART3/DMA/HRTIM FLT）
├── Drivers/                 # STM32G4 HAL + CMSIS
├── cmake/stm32cubemx/       # CubeMX 生成的外设驱动源码
├── pc_host/
│   ├── oled_mirror.py       # 上位机源码（pygame + pyserial）
│   ├── build.bat / OLED_Mirror.spec   # PyInstaller 打包脚本
│   └── dist/OLED_Mirror.exe # 打包好的上位机可执行文件
├── HARDWARE.md              # 硬件架构与引脚映射（单一事实源）
└── CMakeLists.txt           # CMake 构建入口
```

**分层**：业务逻辑（`Pulse`/`uart_comm`/`Preset`）与 CubeMX 生成代码隔离；自定义代码全部位于 `/* USER CODE BEGIN/END */` 保护块内；底层引脚以语义化宏封装（`LOADSW_Pin`、`HRT_CHB2_Pin`、`KEY_TRG_Pin` 等），业务代码不裸写引脚号。

---

## 5. SCPI 控制与 PC 通信协议

### 5.1 链路
- 物理：`USART3`，`PB10=TX / PB11=RX`，**460800 8N1**（默认），TX 走 `DMA1_Channel2`，RX 走 RXNE 逐字节中断。
- 帧格式：`<AA 55 A5> <TYPE> <LEN:LE> <Payload> <CRC16:LE>`，CRC16-CCITT（0x1021, init 0xFFFF）。

| 帧类型 | 值 | 方向 | 含义 |
|:---|:---|:---|:---|
| BTN | 0x01 | PC→MCU | 虚拟按键（载荷=键码序列） |
| CMD | 0x02 | PC→MCU | SCPI ASCII 命令（`\n` 结尾） |
| PING | 0x03 | 双向 | 心跳（PC 每 500ms 发） |
| FRAME | 0x10 | MCU→PC | 屏幕镜像原始 2048B |
| FRAME_RLE | 0x13 | MCU→PC | 屏幕镜像 RLE 压缩 |
| RSP | 0x11 | MCU→PC | SCPI 响应 ASCII |
| ACK | 0x12 | MCU→PC | 握手 / 连接确认 |

连接状态机：收到任一合法 PC 帧 → `LINKED`；2s 无数据 → `IDLE`（仅暂停镜像推流，SCPI/按键仍有效）。

### 5.2 SCPI 命令集（CMD 帧载荷，大小写不敏感，以 `:` 分隔）

| 命令 | 示例 | 说明 |
|:---|:---|:---|
| `*IDN?` | `*IDN?` | 仪器标识（PyVISA 兼容，返回 `PulseGen,G474-PulseGen,0001,1.0`） |
| `OUTP:ON/OFF` | `OUTP:ON` | 输出使能/关闭 |
| `MODE:<m>` | `MODE:COMPPWM` | 切模式：`NPULSE / DPULSE / PWM / NPULSELONG / PWMLONG / COMPPWM / COMPPWMLONG`（同步页面跳转 + Preset） |
| `CHAN:<1..6>` | `CHAN:3` | 选择输出通道（互补模式选通道对），同步屏幕显示 |
| `POL:<0/1>` | `POL:0` | 极性：0=高有效 / 1=低有效，同步屏幕显示 |
| `PULS:WIDTH/COUNT/INTV:<v>` | `PULS:WIDTH:10` | N 脉冲参数（width/interval 单位 µs） |
| `DPULS:PW1/INTV/PW2:<v>` | `DPULS:PW1:5` | 双脉冲参数（单位 µs，整数 1~200） |
| `PWM:PER/DUTY:<v>` | `PWM:DUTY:50` | PWM 周期(µs)/占空比(%) |
| `COMP:PER/DUTY/DTR/DTF:<v>` | `COMP:DTR:100` | 互补 PWM 周期/占空比/上升沿死区/下降沿死区(ns) |
| `LPWM:PER/DUTY:<v>` | `LPWM:PER:2` | PWM Long 周期(s)/占空比(%) |
| `BURST:<prf>` | `BURST:1000` | 猝发重复频率(Hz)，0=单次 |
| `TRIG` | `TRIG` | 单次触发 |
| `12V:ON/OFF` | `12V:OFF` | 12V_OUT 手动开关 |
| `PRESET:SAVE/LOAD` | `PRESET:SAVE` | 保存/调出参数 |
| `STAT` | `STAT` | 查询状态（模式/输出/12V/通道/诊断计数） |
| `HELP` | `HELP` | 返回命令清单 |
| `KEY:<n>` | `KEY:5` | 虚拟按键（1~6 = 上下左右/确定/返回；8/9 = 滚轮上/下） |

> 除按命令查询外，MCU 在 **OUT / 12V / 模式 / 通道** 状态变化时会**主动推送** `STAT` 帧（上位机 0 延迟同步），无需轮询。

---

## 6. 上位机辅助工具（pc_host）

`oled_mirror.py`（或已打包的 `dist/OLED_Mirror.exe`）提供：

1. **1:1 OLED 镜像**：读取 FRAME/RLE 帧，像素级同步显示 128×128 屏幕；诊断行显示实时推流帧率 + 累计帧数 + CRC 失败计数。
2. **虚拟方向键**：等价物理摇杆；鼠标滚轮按页面类型智能分发（菜单=上/下移，数值=增/减），中键=确定、长按=返回。
3. **SCPI 命令行**：发送 CMD 帧并显示 RSP；支持命令历史（↑/↓）+ TAB 补全 + HELP 帮助面板 + RSP 折行。
4. **后台 TCP SCPI 服务端**：监听 `127.0.0.1:5025`，供 PyVISA 等第三方工具直接自动化控制（无需 GUI）。
5. **一键快照**：保存当前镜像为 PNG 到 `screenshots/` 文件夹。
6. **心跳维持**：每 500ms PING，未连接时 MCU 自动暂停推流。
7. **串口/波特率运行时切换** + 高 DPI 缩放 + 欢迎画面：免重启重连。

```bash
# 源码运行（依赖 pygame + pyserial）
pip install pygame pyserial
python oled_mirror.py --port COM5 --baud 460800 --scale 4
# 不带 --port 则启动后从界面选择串口；--demo 无串口演示
```

---

## 7. 编译、烧录与配置

### 7.1 编译
```bash
# CMake 构建（输出目录以实际为准）
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug
# 清理
cmake --build cmake-build-debug --target clean
```
- 工具链：Arm GNU Toolchain（`arm-none-eabi-gcc`），CMake ≥ 3.22。
- 优化：`cmake/gcc-arm-none-eabi.cmake` 已启用极致性能 `-O3 -flto -funroll-loops -ffp-contract=fast`，FPU 单精度硬件浮点 `fpv4-sp-d16 + hard`；FLASH ART 预取缓冲在 `stm32g4xx_hal_msp.c` 显式使能（170MHz@4WS 下隐藏取指等待）。
- 自定义源码在根 `CMakeLists.txt` 的 `target_sources` 中登记；新增 `.c` 文件需同步加入，否则不参与编译。

### 7.2 烧录
- 通过 **SWD**（`PA13=SWDIO / PA14=SWCLK`，J1 接口），使用 ST-Link / J-Link 或 `st-flash` 烧录 `G474-Test.elf/bin`。
- BOOT0 选择（J2）：短接 2-3 → 主 Flash 启动（默认）；短接 1-2 → 系统存储器启动。

### 7.3 关键配置项
| 配置 | 位置 | 说明 |
|:---|:---|:---|
| PC 串口波特率 | `usart.c` `huart3.Init.BaudRate` | 默认 460800（CH340 下 921600 易丢字节） |
| Preset Flash 地址 | `Preset.c` `PRESET_FLASH_ADDR` | 末页 0x0807F800（2KB） |
| SYNC 脉宽 | `Pulse.h` `SYNC_OUT_WIDTH_TICKS` | 1088 tick = 200ns @ MUL32 |
| Burst PRF 上限 | `Pulse.h` `BURST_PRF_MAX_HZ` | 100000 Hz |
| 12V 默认使能 | `main.c` `g_12v_enable` | 默认 `true`（仍受电源有效门控） |

---

## 8. 保护机制与安全设计

- **硬件 Fault 封锁**：`PA15 = HRTIM1_FLT2`（AF13，低有效 + 内部上拉）。触发后 Timer A/B/D 输出由 HRTIM 硬件 ns 级扣到无效电平；FLT2 中断回调再执行 `Pulse_EmergencyStop()` 断 12V 并复位状态机。
- **紧急关断 `Pulse_EmergencyStop()`**：① 先切 12V（`PA12` 拉低）→ ② `ODISR` 关 HRTIM 全部 8 路 + 停 Master/Timer 计数 → ③ 停 TIM5 并拉低长脉冲引脚 → ④ 停 TIM3 PRF、拉低帧标记。**NMI / HardFault / Error_Handler 均调用**。
- **互补防直通**：HRTIM 死区发生器硬件插入（上升/下降沿独立配置）；超长互补用 TIM5 软件死区，所有电平切换走 BSRR 原子写，且初始化/关断先双路拉低。
- **12V 门控**：`LTC_IS_ANY_PWR_VALID() && g_12v_enable` 双条件满足才 `LOADSW_ENABLE()`，掉电/故障微秒级切断。
- **中断优先级分级**（发波关键路径优先）：EXTI1 触发 = 最高(0,0) → TIM5 长脉冲(1,0) → TIM3 PRF(1,0) → TIM7/TIM16 按键状态(2,x) → OLED SPI/DMA、TIM6、USART3(3,x) 最低，绝不阻塞发波。
- **串口风暴保护**：RXNE 中断计数超阈值自动关闭接收中断 500ms，防噪声中断风暴拖死主循环。

---

## 9. 开源组件与致谢

- **UI 框架**：本项目 OLED 菜单系统基于 [WouoUI-PageVersion](https://github.com/Sheep118/WouoUI-PageVersion)（WouoUI Page 版本，作者 Sheep118），提供列表 / 弹窗 / 数值编辑等交互控件；上游原始框架为 [RQNG/WouoUI](https://github.com/RQNG/WouoUI)。
