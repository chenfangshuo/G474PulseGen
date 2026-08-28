# 5V_SigGen_for_DRV_Board 硬件架构与引脚映射规范手册 (HARDWARE.md)

> 本文档已对照 `Netlist Schematic.NET` 校正。阻值（如 20Ω / 10k / 4.7k）网表中未给出，仍按原理图标注沿用，贴片以 BOM 为准。

---

## 1. 硬件平台概况

- **主控芯片 (MCU)**: STM32G474CET6 (Arm® Cortex®-M4 @ 170MHz, 512KB Flash, 128KB SRAM, LQFP48 封装)
- **板卡定位**: 高精度 5V 脉冲信号发生器 / 功率驱动信号板 (5V SigGen for DRV Board)
- **时钟系统**: 外部无源晶振 24.000MHz (X1), PLL 倍频至 170MHz 系统主频 (HRTIM 运行于 170MHz x 32 = 5.44GHz 极高时间分辨率模式)
- **电源拓扑**:
  - **输入源 1 (USB-C PD)**: Type-C (USB1) VBUS 经功率 MOS **Q1 (BMS013TN4D)** 进入 IP2721 (U1)，诱骗适配器输出 12V 至 `PWR1_TRIGD_12V`
  - **输入源 2 (2EDG 外部端子)**: CN1 2-Pin 5.08mm 接线端子经 R7 输入 12V (`PWR2_2EDG_12V`)
  - **电源多路复用切换**: LTC4421CG (U2) 双通道优先/理想二极管控制器输出 `MUXED_12V`
  - **外部关断接口**: 板载 `PWR` (SPPH420100) 接到 LTC4421 `SHDN#`，可外部拉低关断电源路径
  - **主供电线性稳压**（非开关 buck）:
    - `MUXED_12V` -> NCV7805 (U3, 线性稳压) -> 5.0V 系统供电 (`5V`)
    - `5V` -> TLV76133 (U4) -> 3.3V 数字核心供电 (`3.3V`)
  - **负载功率输出**: TPS22810 (U9 & U10 并联) 负载开关受 MCU `PA12` 控制（板载 R57 下拉，上电默认关断），输出 `12V_OUT` 至 J8 接口

---

## 2. STM32G474CET6 (LQFP48) 全引脚映射表

| 引脚编号 | 引脚名称 | 原理图网络名 (Net Name) | 默认配置/复用功能 | 连接器件及电气说明 |
|:---|:---|:---|:---|:---|
| **1** | VBAT | 3.3V | 电源 | 接入 3.3V 数字供电 |
| **2** | PC13 | PC13/K_UP | GPIO_Input (Pull-None) | 五向摇杆 UP 键 (经 MAX6818 U6 Pin 19 输出) |
| **3** | PC14-OSC32_IN | PC14/PWR1_DT | GPIO_Input / DET1 | 电源通道 1 状态检测 (MOS Q6 漏极，LTC4421 CH1#；R34 上拉至 3.3V) |
| **4** | PC15-OSC32_OUT| PC15/PWR2_DT | GPIO_Input / DET2 | 电源通道 2 状态检测 (MOS Q7 漏极，LTC4421 CH2#；R35 上拉至 3.3V) |
| **5** | PF0-OSC_IN | NetC30_2 | RCC_OSC_IN | 24MHz 外部高速晶振输入 |
| **6** | PF1-OSC_OUT | NetC31_2 | RCC_OSC_OUT | 24MHz 外部高速晶振输出 |
| **7** | PG10/NRST | NetC32_2 | NRST / RESET | 系统复位引脚，接 RESET 按钮 (SKRPABE010) 与 RC 滤波 |
| **8** | PA0 | PA0/ENC_A | TIM2_CH1 (Encoder A) | 旋转编码器 A 相；C34 对地 + R37 上拉至 3.3V |
| **9** | PA1 | PA1/ENC_B | TIM2_CH2 (Encoder B) | 旋转编码器 B 相；C35 对地 + R38 上拉至 3.3V |
| **10** | PA2 | PA2/RS232_TX | USART2_TX (外设复用) | RS232 串口发送，经 R39 连接 MAX3232 (U7 Pin 10 DIN2) |
| **11** | PA3 | PA3/RS232_RX | USART2_RX (外设复用) | RS232 串口接收，经 R40 连接 MAX3232 (U7 Pin 9 ROUT2) |
| **12** | PA4 | PA4/K_DOWN | GPIO_Input (Pull-None) | 五向摇杆 DOWN 键 (经 MAX6818 U6 Pin 18 输出) |
| **13** | PA5 | PA5/K_LEFT | GPIO_Input (Pull-None) | 五向摇杆 LEFT 键 (经 MAX6818 U6 Pin 17 输出) |
| **14** | PA6 | PA6/K_RIGHT| GPIO_Input (Pull-None) | 五向摇杆 RIGHT 键 (经 MAX6818 U6 Pin 16 输出) |
| **15** | PA7 | PA7/K_CENTER| GPIO_Input (Pull-None)| 五向摇杆 CENTER(PRESS) 键 (经 MAX6818 U6 Pin 15) |
| **16** | PB0 | PB0/K_ENC | GPIO_Input (Pull-None) | 旋转编码器按键 (经 MAX6818 U6 Pin 14 输出) |
| **17** | PB1 | PB1/K_TRG | GPIO_EXTI1 (外部中断) | 硬件触发按键 / 外部触发输入 (经 MAX6818 U6 Pin 13) |
| **18** | PB2 | PB2 | GPIO / Expansion | J6 Pin 4；R45 上拉至 3.3V、R48 下拉至 GND（以实际贴片为准） |
| **19** | VSSA | GND | 电源 | 模拟地 |
| **20** | VREF+ | 3.3V | 电源 | ADC/DAC 参考电压正，接 3.3V |
| **21** | VDDA | 3.3V | 电源 | 模拟供电正，接 3.3V |
| **22** | PB10 | PB10/BLE_TX | USART3_TX (或 GPIO) | 蓝牙模块串口发送 (连接 J5 Pin 2) |
| **23** | VSS | GND | 电源 | 数字地 |
| **24** | VDD | 3.3V | 电源 | 数字供电正，接 3.3V |
| **25** | PB11 | PB11/BLE_RX | USART3_RX (或 GPIO) | 蓝牙模块串口接收 (连接 J5 Pin 3) |
| **26** | PB12 | PB12/HRT_C1 | HRTIM1_CHC1 | Timer C CH1：经 **R61** 至 U8 Pin 10 (A8)；另经 **R63** 并联至 Y8 输出网 |
| **27** | PB13 | PB13/HRT_C2 | HRTIM1_CHC2 | Timer C CH2：经 **R60** 至 U8 Pin 9 (A7)；另经 **R62** 并联至 Y7 输出网 |
| **28** | PB14 | PB14/HRT_D1 | HRTIM1_CHD1 | 高精定时器 Timer D 通道 1 -> 74LVCH8T245 (U8 Pin 8 A6) |
| **29** | PB15 | PB15/HRT_D2 | HRTIM1_CHD2 | 高精定时器 Timer D 通道 2 -> 74LVCH8T245 (U8 Pin 7 A5) |
| **30** | PA8 | PA8/HRT_A1 | HRTIM1_CHA1 | 高精定时器 Timer A 通道 1 -> 74LVCH8T245 (U8 Pin 6 A4) |
| **31** | PA9 | PA9/HRT_A2 | HRTIM1_CHA2 | 高精定时器 Timer A 通道 2 -> 74LVCH8T245 (U8 Pin 5 A3) |
| **32** | PA10 | PA10/HRT_B1 | HRTIM1_CHB1 | 高精定时器 Timer B 通道 1 -> 74LVCH8T245 (U8 Pin 4 A2) |
| **33** | PA11 | PA11/HRT_B2 | HRTIM1_CHB2 | 高精定时器 Timer B 通道 2 -> 74LVCH8T245 (U8 Pin 3 A1) |
| **34** | PA12 | PA12/LOADSW | GPIO_Output_PP | 功率负载开关使能 (U9/U10 EN)；**R57 下拉至 GND**，上电默认关断 |
| **35** | VSS | GND | 电源 | 数字地 |
| **36** | VDD | 3.3V | 电源 | 数字供电正，接 3.3V |
| **37** | PA13 | PA13/SWDIO | SYS_JTMS-SWDIO | SWD 调试数据线，接 J1 Pin 3 (R31 上拉至 3.3V) |
| **38** | PA14 | PA14/SWCLK | SYS_JTCK-SWCLK | SWD 调试时钟线，接 J1 Pin 2 (R32 下拉至 GND) |
| **39** | PA15 | PA15/I2C1_SCL| I2C1_SCL | I2C1 时钟线 → J6 Pin 2；R43 上拉至 3.3V、R46 下拉至 GND |
| **40** | PB3 | PB3/SPI1_SCK | SPI1_SCK | OLED 硬件 SPI 时钟，连接 J4 Pin 5 |
| **41** | PB4 | PB4/SPI1_CS | GPIO_Output_PP (CS) | OLED 片选信号，连接 J4 Pin 1 (低有效) |
| **42** | PB5 | PB5/SPI1_MOSI| SPI1_MOSI | OLED 硬件 SPI 数据，连接 J4 Pin 4 |
| **43** | PB6 | PB6/SPI1_DC | GPIO_Output_PP (DC) | OLED 数据/命令选择，连接 J4 Pin 2 (高:数据, 低:命令) |
| **44** | PB7 | PB7/SPI1_RES | GPIO_Output_PP (RES) | OLED 硬件复位信号，连接 J4 Pin 3 (低有效) |
| **45** | PB8-BOOT0 | PB8/BOOT0 | BOOT0 / GPIO | 经 **R30** 接 J2 中间脚；跳线至 Pin1 (3.3V) 或 Pin3 (GND) |
| **46** | PB9 | PB9/I2C1_SDA | I2C1_SDA | I2C1 数据线 → J6 Pin 3；R44 上拉至 3.3V、R47 下拉至 GND |
| **47** | VSS | GND | 电源 | 数字地 |
| **48** | VDD | 3.3V | 电源 | 数字供电正，接 3.3V |


---

## 3. 人机交互系统 (按键 / 编码器 / 硬件消抖)

### 3.1 MAX6818 硬件消抖芯片 (U6) 映射
板载专用 8 通道 CMOS 开关消抖芯片 **MAX6818EAP+T** (U6, SSOP-20)，`EN#` 接地常使能；具备 ±15kV ESD 防静电保护与精确 40ms 消抖窗口，直接消除机械抖动并输出干净的数字电平。IN8/OUT8 未使用。

| 输入信号 (按键端) | U6 输入引脚 (INx) | U6 输出引脚 (OUTx) | MCU 连接引脚 | 逻辑说明 |
|:---|:---|:---|:---|:---|
| `KEY_UP` | Pin 2 (IN1) | Pin 19 (OUT1) | **PC13** (`PC13/K_UP`) | 按下时低电平 (0) |
| `KEY_DOWN` | Pin 3 (IN2) | Pin 18 (OUT2) | **PA4** (`PA4/K_DOWN`) | 按下时低电平 (0) |
| `KEY_LEFT` | Pin 4 (IN3) | Pin 17 (OUT3) | **PA5** (`PA5/K_LEFT`) | 按下时低电平 (0) |
| `KEY_RIGHT` | Pin 5 (IN4) | Pin 16 (OUT4) | **PA6** (`PA6/K_RIGHT`)| 按下时低电平 (0) |
| `KEY_CENTER` | Pin 6 (IN5) | Pin 15 (OUT5) | **PA7** (`PA7/K_CENTER`)| 按下时低电平 (0) |
| `KEY_ENC` | Pin 7 (IN6) | Pin 14 (OUT6) | **PB0** (`PB0/K_ENC`) | 编码器中键按下为低 (0) |
| `KEY_TRG` | Pin 8 (IN7) | Pin 13 (OUT7) | **PB1** (`PB1/K_TRG`) | 触发键/EXT_TRG 下降沿 (0) |

> **注**: MCU 引脚无需使能内部上下拉（`GPIO_NOPULL`），MAX6818 内部具备上拉与推挽输出。

### 3.2 旋转编码器 (EC12D)
- **A 相**: 连接至 MCU **PA0** (`TIM2_CH1`)；C34 对地电容 + R37 上拉至 3.3V（非串联 RC）。
- **B 相**: 连接至 MCU **PA1** (`TIM2_CH2`)；C35 对地电容 + R38 上拉至 3.3V。
- **按键 (E)**: 连接至 `KEY_ENC` -> MAX6818 (U6 Pin 7) -> **PB0**。

### 3.3 触发输入系统 (Trigger Input)
- **板载触发按键**: `SW_TRG` (SKHHQYA010 按键)，按下接地。
- **外部触发接口**: `J3` (EXT_TRG 1x2 Pin 2.54mm Header)，Pin 1 信号，Pin 2 GND。
- **MCU 响应**: 汇集至 `KEY_TRG` -> MAX6818 -> **PB1** (`EXTI1_IRQn`，下降沿触发中断)。

---

## 4. 显示与通信接口

### 4.1 OLED 显示屏接口 (J4 - 7-Pin SPI 0.96/1.3寸 OLED)
| OLED Pin | 引脚功能 | MCU 连接 | 驱动配置模式 |
|:---|:---|:---|:---|
| **Pin 1** | CS (片选) | **PB4** | GPIO Output (推挽/高速)，软件控制 |
| **Pin 2** | DC (数据/命令) | **PB6** | GPIO Output (推挽/高速)，0:CMD, 1:DATA |
| **Pin 3** | RES (硬件复位) | **PB7** | GPIO Output (推挽/高速)，低电平复位 |
| **Pin 4** | SDA/MOSI (数据) | **PB5** | SPI1_MOSI (DMA 发送) |
| **Pin 5** | SCL/SCK (时钟) | **PB3** | SPI1_SCK (波特率预分频 8 / ~10.625Mbps) |
| **Pin 6** | VCC (供电) | **3.3V** | 3.3V 稳压电源 |
| **Pin 7** | GND (地) | **GND** | 系统参考地 |

### 4.2 RS232 串口接口 (J_RS232 / MAX3232 U7)
- **电平转换芯片**: MAX3232EIPWR (U7, TSSOP-16, 3.3V 供电)。
- **MCU 连接 (使用 Channel 2)**: 
  - **PA2 (TX)** -> R39 -> U7 Pin 10 (`DIN2`) -> U7 Pin 7 (`DOUT2`) -> R41 -> RS232 Pin 3
  - **PA3 (RX)** <- R40 <- U7 Pin 9 (`ROUT2`) <- U7 Pin 8 (`RIN2`) <- R42 <- RS232 Pin 2
- **物理接口**: 1x3 Pin Header (`RS232`), Pin 1: GND, Pin 2: RX, Pin 3: TX。

### 4.3 蓝牙接口 (J5 - BLE 6-Pin Header)
网表中 **仅 4 个引脚有网络**。模块供电必须使用 Pin 6，**不可按 Pin 5 供电**。

| J5 Pin | 网络 / 连接 | 说明 |
|:---|:---|:---|
| **Pin 1** | 未连接 (NC) | 悬空，无 STATE 信号 |
| **Pin 2** | MCU **PB10 / BLE_TX** | 接模块 RXD |
| **Pin 3** | MCU **PB11 / BLE_RX** | 接模块 TXD |
| **Pin 4** | GND | 系统地 |
| **Pin 5** | 未连接 (NC) | **无 5V、无 3.3V** |
| **Pin 6** | **3.3V** | 板载唯一蓝牙供电脚 |

### 4.4 I2C / 扩展接口 (J6 & J7)
- **J6 (5-Pin)**:
  - Pin 1: 3.3V
  - Pin 2: **PA15** (`I2C1_SCL`)；R43 上拉至 3.3V，R46 下拉至 GND
  - Pin 3: **PB9** (`I2C1_SDA`)；R44 上拉至 3.3V，R47 下拉至 GND
  - Pin 4: **PB2** (通用 GPIO)；R45 上拉至 3.3V，R48 下拉至 GND
  - Pin 5: GND
  - 每组“上拉 + 下拉”为可选贴片，实际只应焊接其中一侧（或均不贴而改用 MCU 内部上下拉）。I2C 常用配置为仅贴上拉（R43/R44）。
- **J7 (5-Pin)**:
  - Pin 1: 5V
  - Pin 2: 5V
  - Pin 3: 3.3V
  - Pin 4: 3.3V
  - Pin 5: GND

### 4.5 BOOT0 选择 (J2)
- **J2 Pin 1**: 3.3V
- **J2 Pin 2**: 经 **R30** 接 MCU **PB8-BOOT0**
- **J2 Pin 3**: GND
- 跳线帽短接 1-2：BOOT0 = 高（系统存储启动）；短接 2-3：BOOT0 = 低（主 Flash 启动）。R30 始终串在 MCU 与跳线中间脚之间。

---

## 5. 高速脉冲驱动与输出系统 (HRTIM & 74LVCH8T245)

### 5.1 74LVCH8T245 电平转换器 (U8) 与 SMA 输出映射
板载高速双电源电平转换芯片 **74LVCH8T245PW** (U8, TSSOP-24)：

- **VCCA** (Pin 1) 接 **3.3V**；**DIR** (Pin 2) 接 **3.3V**（A→B）；**OE#** (Pin 22) 接地常开。
- **VCCB** (Pin 23/24) **不是死接到 5V**，而是网络 `NetC42_2`：经 **R64** 到 `5V`、经 **R65** 到 `3.3V`。仅贴 R64、不贴 R65 时 B 侧为 5V 输出；反之则为 3.3V 输出。**禁止两颗同时焊接。**

Y1–Y6 为 `SMA_Conn`（信号在 Pin 2）；Y7/Y8 为 `SMA-KE`（信号在 Pin 5，Pin 1–4 接地）。

| 定时器通道 | MCU 引脚 | 至 U8 A 侧路径 | U8 输入 | U8 输出 | B 侧串联电阻 | SMA |
|:---|:---|:---|:---|:---|:---|:---|
| **HRTIM1_CHA1** | **PA8** | 直连 | Pin 6 (A4) | Pin 18 (B4) | R52 | **Y4** |
| **HRTIM1_CHA2** | **PA9** | 直连 | Pin 5 (A3) | Pin 19 (B3) | R51 | **Y3** |
| **HRTIM1_CHB1** | **PA10** | 直连 | Pin 4 (A2) | Pin 20 (B2) | R50 | **Y2** |
| **HRTIM1_CHB2** | **PA11** | 直连 | Pin 3 (A1) | Pin 21 (B1) | R49 | **Y1** |
| **HRTIM1_CHC1** | **PB12** | **经 R61** | Pin 10 (A8) | Pin 14 (B8) | R56 | **Y8** |
| **HRTIM1_CHC2** | **PB13** | **经 R60** | Pin 9 (A7) | Pin 15 (B7) | R55 | **Y7** |
| **HRTIM1_CHD1** | **PB14** | 直连 | Pin 8 (A6) | Pin 16 (B6) | R54 | **Y6** |
| **HRTIM1_CHD2** | **PB15** | 直连 | Pin 7 (A5) | Pin 17 (B5) | R53 | **Y5** |

**Timer C（Y7/Y8）额外并联路径（网表事实）**：

- **R63**：一端接 `PB12/HRT_C1`，另一端接 Y8 信号网（与 R56 输出同网）
- **R62**：一端接 `PB13/HRT_C2`，另一端接 Y7 信号网（与 R55 输出同网）

若 R62/R63 焊接，MCU 3.3V 脚会与 U8 的 B 侧输出并联。正常经电平转换输出时应 **不贴 R62/R63**（或仅作调试旁路）。R49–R56 阻值以 BOM 为准（原理图常标 20Ω）。

### 5.2 功率负载开关 (TPS22810 U9 & U10)
- **控制引脚**: **PA12** (`PA12/LOADSW`)，推挽输出；**R57 下拉至 GND**，复位后保持关断。
- **驱动拓扑**: 2 片 **TPS22810DBVR** 并联，输入为 `MUXED_12V`，输出为 `12V_OUT`。
- **控制逻辑**:
  - `PA12 = 1 (HIGH)`: 负载开关导通，`12V_OUT` 向 J8 (`PWR_OUT`) 供电。
  - `PA12 = 0 (LOW)`: 负载开关关断；QOD 经 R58/R59 接到 `12V_OUT`，由芯片内部放电通路将输出泄放至 GND。
- **输出接口 (J8)**: 2x4 双排母座，Pin 1~4 为 GND，Pin 5~8 为 `12V_OUT`。

---

## 6. 电源管理与双路 LTC4421 理想二极管控制器

### 6.1 LTC4421CG (U2) 架构
板载 LTC4421 双通道优先电源控制器，管理两路 12V 输入源并驱动背靠背 N-MOSFET (Q2/Q3 与 Q4/Q5)：
1. **通道 1 (V1)**: `PWR1_TRIGD_12V` — USB-C VBUS 经 **Q1** 与 IP2721 (U1) 诱骗后的 12V
2. **通道 2 (V2)**: `PWR2_2EDG_12V` — CN1 经 R7 输入的 12V
3. **输出 (OUT)**: `MUXED_12V` -> NCV7805 (5V 线性稳压) 与 TPS22810 功率输出
4. **SHDN#**: 接板载 `PWR` 连接器 (SPPH420100 Pin 2/5) 及上拉网络；外部可将 `SHDN#` 拉低以关断 LTC4421

### 6.2 状态监测与指示灯
- **MCU 状态检测**:
  - **CH1# 状态**: 经 N-MOS Q6 倒相后接入 MCU **PC14** (`PC14/PWR1_DT`，高电平表示 CH1 导通)。
  - **CH2# 状态**: 经 N-MOS Q7 倒相后接入 MCU **PC15** (`PC15/PWR2_DT`，高电平表示 CH2 导通)。
- **板载状态指示 LED**:
  - `D5`: V1 故障告警指示 (LTC4421 `FAULT1#`，低电平点亮)
  - `D6`: V1 电压正常指示 (LTC4421 `VALID1#`，低电平点亮)
  - `D7`: V1 通道导通指示 (LTC4421 `CH1#`，低电平点亮)
  - `D8`: V2 故障告警指示 (LTC4421 `FAULT2#`，低电平点亮)
  - `D9`: V2 电压正常指示 (LTC4421 `VALID2#`，低电平点亮)
  - `D10`: V2 通道导通指示 (LTC4421 `CH2#`，低电平点亮)

---

## 7. 嵌入式软件开发关键注意事项 (Checklist)

1. **HRTIM 发波与互补防直通**:
   - 当使用 HRTIM 生成半桥/全桥驱动信号时，必须确保配置死区时间生成器 (Dead-Time Insertion, DTR/DTF)，严禁上下桥同臂直通短路。
   - STM32G474 HRTIM 必须在启动时执行 DLL 硬件自动校准 (`HAL_HRTIM_DLLCalibrationStart`)。
   - Timer C (PB12/PB13 → Y8/Y7) 走线与 A/B/D 不同，软件通道映射不变，但默认按经 U8 转换输出理解（R62/R63 不贴）。
2. **脉冲输出电平**:
   - 5V 逻辑取决于 U8 VCCB 选择电阻：**仅 R64** → 5V；**仅 R65** → 3.3V。软件无法改变该硬件选择。
3. **CubeMX 代码保护规则**:
   - 用户自定义宏定义、中断回调、业务逻辑一律放置于 `/* USER CODE BEGIN xxx */` 与 `/* USER CODE END xxx */` 注释块内。
4. **EXTI 外部触发中断**:
   - `PB1` 配置为 `EXTI_Line1` 下降沿触发中断（NVIC 优先级 0, 0），用于硬件脉冲极速响应。
5. **OLED SPI DMA 发送**:
   - OLED 刷新采用 `SPI1` + `DMA1_Channel1` 模式，发送前需将 `PB6 (DC)` 置高（数据）或置低（命令），并将 `PB4 (CS)` 拉低。
6. **功率负载开关时序**:
   - 硬件 R57 已将 `PA12` 拉低。软件初始化时仍须将 `PA12 (LOADSW)` 保持 `RESET (LOW)`，待系统稳定、外设就绪后再使能。
7. **蓝牙供电**:
   - 仅 J5 Pin 6 提供 3.3V；Pin 5 悬空。USART3：PB10=TX，PB11=RX。
8. **I2C 上下拉**:
   - 确认 R43/R44（上拉）与 R46/R47（下拉）的实际贴片后再决定是否开启 MCU 内部上下拉。
