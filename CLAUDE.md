# 语言与项目规范
- 所有交互、代码分析、终端输出解释及注释一律使用【简体中文】。
- 架构领域：精通 STM32 架构（STM32G4/F4 等）、HAL 库以及高频数字电源/电机控制。

# 常用终端构建指令 (Claude Code 自主验证使用)
- 编译项目：`cmake --build build/Debug`
- 清理项目：`cmake --build build/Debug --target clean`

# 硬件单一事实源（Single Source of Truth）
- 严格遵循根目录 `HARDWARE.md`：所有 GPIO 引脚、HRTIM 通道映射、USART/SPI/I2C 外设以及外部跳线配置，必须 100% 严格以 `HARDWARE.md` 为唯一基准，严禁任何主观推测或使用通用例程默认引脚。
- 宏定义语义化封装：底层驱动优先在对应头文件中将引脚与端口封装为带语义的宏（如 `LOADSW_PIN`, `LTC_CH1_DET_PORT`），业务逻辑中禁止裸写引脚编号。
- 电气特性与逻辑遵循：严格按照 `HARDWARE.md` 中标注的有效电平（如按键低有效、MAX6818 输出逻辑、LTC4421 状态检测反相特性）编写逻辑。

# CubeMX 代码保护规则（核心防破坏）
- 严格遵循 CubeMX 代码隔离规范：所有自定义代码必须且只能写在 `/* USER CODE BEGIN xxx */` 与 `/* USER CODE END xxx */` 之间，严禁修改保护区之外的自动生成代码。
- 保持既有外设句柄命名一致性（如 `hhrtim1`, `hspi1`, `huart2`, `huart3`）及工程既有分层架构。

# 嵌入式与硬件安全规范
- 发波与硬件安全：编写 HRTIM/PWM、互补输出、死区时间（DTR/DTF）时，必须优先防范上下桥直通短路；涉及 HRTIM 时确保包含启动 DLL 硬件自动校准逻辑。
- 类型规范：一律使用标准整型（`uint8_t`, `uint16_t`, `uint32_t`, `int32_t` 等）。
- 寄存器与时钟安全：涉及直接操作寄存器（如 `TIMx->CCR1`, `HRTIMx->sTimerxRegs`）时，必须附带简明中文行内注释解释计数值与时序含义。

# Agent 行为准则
- 跨文件修改前，先简要说明执行步骤（Plan）并核对与 `HARDWARE.md` 的引脚一致性。
- 代码修改完成后，自主调用 CMake 编译命令验证是否存在编译/链接报错并修复；完成后提供一键核对清单（Checklist）。