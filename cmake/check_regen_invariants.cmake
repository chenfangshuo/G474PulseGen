# =============================================================================
# CubeMX 重新生成防线
#
# 用法:  cmake -P cmake/check_regen_invariants.cmake
#        (由 CMakeLists.txt 作为构建前置步骤自动调用, 无需手动跑)
#
# 背景
# ----
# 本工程有三样东西游离于 .ioc 之外, CubeMX 重新生成时会当成"不该存在"而拆掉:
#   1. USART3 + 其 TX DMA  (工程初始就没在 .ioc 里)
#   2. TICK_INT_PRIORITY = 0  (提交 733c9b8 修并发问题时手工改的)
#   3. HAL_UART_MODULE_ENABLED (被 USART3 依赖)
#
# 2026-09 的一次重新生成实际发生了: 构建报出 21 个错误, 且 **TICK_INT_PRIORITY
# 被静默改回 15** —— 后者不报错、能编译能运行, 只是把中断优先级反转与 ISR 死锁
# 的前提条件悄悄恢复了。这类静默回退比编译失败危险得多, 因此需要自动防线。
#
# 分两级
# ------
#   FATAL    : 出现即说明本次重新生成已经破坏了工程 (或构建即将失败)。退出码非 0。
#   ADVISORY : .ioc 侧的待办。不阻断构建, 只提示 —— 在 .ioc 里补齐后自然消失。
#
# ⚠ 正则写法陷阱 (踩过)
# --------------------
# 在 CMake 脚本模式下, 正则里**不要用 \\( 去匹配字面括号** —— 它不会按预期生效,
# 结果是检查静默误报 FAIL。一律用 . 通配代替括号与点号: 这些检查的目的是"确认某段
# 代码存在", 不需要精确匹配标点。
# =============================================================================

if(NOT DEFINED PROJECT_ROOT)
    get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(_failed   0)
set(_advisory 0)

# ---- FATAL 检查: 匹配成功 = 正常 ------------------------------------------
macro(require_match FILEPATH PATTERN LABEL WHY)
    if(EXISTS "${PROJECT_ROOT}/${FILEPATH}")
        file(READ "${PROJECT_ROOT}/${FILEPATH}" _content)
        string(REGEX MATCH "${PATTERN}" _hit "${_content}")
        if(_hit)
            message(STATUS "  [OK]   ${LABEL}")
        else()
            message(STATUS "  [FAIL] ${LABEL}")
            message(STATUS "         ${WHY}")
            set(_failed 1)
        endif()
    else()
        message(STATUS "  [FAIL] ${LABEL}  -- 文件不存在: ${FILEPATH}")
        set(_failed 1)
    endif()
endmacro()

# ---- FATAL 反查: 匹配成功 = 异常 -------------------------------------------
# 用于"某个东西不应该存在"的检查 (例如某宏不得被定义)。
macro(require_not_match FILEPATH PATTERN LABEL WHY)
    if(EXISTS "${PROJECT_ROOT}/${FILEPATH}")
        file(READ "${PROJECT_ROOT}/${FILEPATH}" _content)
        string(REGEX MATCH "${PATTERN}" _hit "${_content}")
        if(_hit)
            message(STATUS "  [FAIL] ${LABEL}")
            message(STATUS "         ${WHY}")
            set(_failed 1)
        else()
            message(STATUS "  [OK]   ${LABEL}")
        endif()
    else()
        message(STATUS "  [FAIL] ${LABEL}  -- 文件不存在: ${FILEPATH}")
        set(_failed 1)
    endif()
endmacro()

# ---- ADVISORY 反查: 匹配成功只提示 -----------------------------------------
macro(advise_not_match FILEPATH PATTERN LABEL TODO)
    if(EXISTS "${PROJECT_ROOT}/${FILEPATH}")
        file(READ "${PROJECT_ROOT}/${FILEPATH}" _content)
        string(REGEX MATCH "${PATTERN}" _hit "${_content}")
        if(_hit)
            message(STATUS "  [待办] ${LABEL}")
            message(STATUS "         ${TODO}")
            set(_advisory 1)
        else()
            message(STATUS "  [OK]   ${LABEL}")
        endif()
    else()
        message(STATUS "  [待办] ${LABEL}  -- 文件不存在: ${FILEPATH}")
        set(_advisory 1)
    endif()
endmacro()

# ---- ADVISORY 检查: 未匹配只提示 -------------------------------------------
macro(advise_match FILEPATH PATTERN LABEL TODO)
    if(EXISTS "${PROJECT_ROOT}/${FILEPATH}")
        file(READ "${PROJECT_ROOT}/${FILEPATH}" _content)
        string(REGEX MATCH "${PATTERN}" _hit "${_content}")
        if(_hit)
            message(STATUS "  [OK]   ${LABEL}")
        else()
            message(STATUS "  [待办] ${LABEL}")
            message(STATUS "         ${TODO}")
            set(_advisory 1)
        endif()
    else()
        message(STATUS "  [待办] ${LABEL}  -- 文件不存在: ${FILEPATH}")
        set(_advisory 1)
    endif()
endmacro()

message(STATUS "")
# ---- FATAL 数值比对: 两个文件里提取出的数值必须相等 ------------------------
# 用于"同一个事实被两处独立维护"的场景。与上面四个宏的区别是它做的是**数值
# 比对**而非模式匹配 —— 模式匹配只能回答"某个东西在不在", 回答不了"两处的值
# 是否一致"。
#
# 两侧任一提取失败也判失败: 提取不到说明文件结构变了, 检查规则本身已失效,
# 不能让构建悄悄通过 —— 否则这道防线会静默作废, 比没有检查更危险。
macro(require_equal_values FILE_A PATTERN_A LABEL_A FILE_B PATTERN_B LABEL_B WHY)
    set(_va "")
    set(_vb "")
    if(EXISTS "${PROJECT_ROOT}/${FILE_A}")
        file(READ "${PROJECT_ROOT}/${FILE_A}" _ca)
        string(REGEX MATCH "${PATTERN_A}" _ma "${_ca}")
        if(CMAKE_MATCH_1)
            set(_va "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(EXISTS "${PROJECT_ROOT}/${FILE_B}")
        file(READ "${PROJECT_ROOT}/${FILE_B}" _cb)
        string(REGEX MATCH "${PATTERN_B}" _mb "${_cb}")
        if(CMAKE_MATCH_1)
            set(_vb "${CMAKE_MATCH_1}")
        endif()
    endif()

    if(_va STREQUAL "" OR _vb STREQUAL "")
        message(STATUS "  [FAIL] ${LABEL_A} 与 ${LABEL_B} 一致")
        message(STATUS "         提取失败: ${FILE_A} -> '${_va}' / ${FILE_B} -> '${_vb}'")
        message(STATUS "         文件结构可能已变, 请更新 check_regen_invariants.cmake 里的正则。")
        set(_failed 1)
    elseif(NOT _va STREQUAL _vb)
        message(STATUS "  [FAIL] ${LABEL_A} 与 ${LABEL_B} 一致")
        message(STATUS "         ${LABEL_A} = ${_va}   (${FILE_A})")
        message(STATUS "         ${LABEL_B} = ${_vb}   (${FILE_B})")
        message(STATUS "         ${WHY}")
        set(_failed 1)
    else()
        message(STATUS "  [OK]   ${LABEL_A} 与 ${LABEL_B} 一致 (${_va})")
    endif()
endmacro()

message(STATUS "=== CubeMX 重新生成不变量检查 ===")
message(STATUS "--- FATAL: 出现即说明重新生成已破坏工程 ---")

# TICK_INT_PRIORITY 必须为 0 (提交 733c9b8 的并发修复)
require_match("Core/Inc/stm32g4xx_hal_conf.h"
    "#define[ \t]+TICK_INT_PRIORITY[ \t]+.0UL."
    "SysTick 优先级 = 0"
    "被改回 15 会静默恢复中断优先级反转/ISR 死锁的前提条件 (见提交 733c9b8)。")

# HAL_UART 模块必须启用 (USART3 依赖)
require_match("Core/Inc/stm32g4xx_hal_conf.h"
    "\n#define[ \t]+HAL_UART_MODULE_ENABLED"
    "HAL_UART 模块已启用"
    "被注释掉会导致 UART_HandleTypeDef 类型消失, 构建直接失败。")

# usart.c 必须参与编译
require_match("cmake/stm32cubemx/CMakeLists.txt"
    "Core/Src/usart.c"
    "usart.c 在编译列表中"
    "usart.c 只在 MX_Application_Src 中列出; 被移除会导致 huart3 / MX_USART3_UART_Init 未定义。")

# USART3 必须被初始化 (main.c 中应有一次调用, 后跟分号)
require_match("Core/Src/main.c"
    "MX_USART3_UART_Init[ \t]*.*;"
    "main() 调用了 MX_USART3_UART_Init()"
    "调用被删则 USART3 永不初始化, 上位机链路完全失效 (且构建仍通过)。")

# g_12v_enable 声明必须存在
require_match("Core/Inc/main.h"
    "extern[ \t]+volatile[ \t]+bool[ \t]+g_12v_enable[ \t]*;"
    "main.h 声明了 g_12v_enable"
    "12V 互锁的跨文件声明。注意: 必须写在 USER CODE BEGIN EFP 内 —— 自造的标记名会被丢弃。")

# 自研中断处理函数必须存在 (读的是定义文件 it.c, 故匹配 void 前缀即为定义)
require_match("Core/Src/stm32g4xx_it.c"
    "void[ \t]+USART3_IRQHandler[ \t]*."
    "USART3_IRQHandler 存在"
    "自研 ISR。删除后 USART3 无法收字节 (RXNE 中断无人处理)。")

require_match("Core/Src/stm32g4xx_it.c"
    "void[ \t]+DMA1_Channel2_IRQHandler[ \t]*."
    "DMA1_Channel2_IRQHandler 存在"
    "USART3_TX 的 DMA 完成中断。缺失会导致发送链停摆。")

# USART3 不得开启 Overrun Disable —— 与 TICK_INT_PRIORITY 同类的静默行为回退
require_not_match("Core/Src/usart.c"
    "UART_ADVFEATURE_OVERRUN_DISABLE"
    "USART3 未开启 Overrun Disable"
    "开启后 ORE 标志永不置位, uart_comm.c 中基于 ORE 的风暴保护 (使能 UART_IT_ORE / UartComm_OreEvent 计数 / 风暴时同关 RXNE+ORE) 与 SCPI STAT 的 ORE 诊断全部失效, 且溢出时数据静默丢失。请在 CubeMX 的 USART3 -> Advanced Features 中取消勾选 Overrun Disable 与 DMA Disable on RX error。")

# ---- HRTIM 计数时钟基准: .ioc 的权威值 必须等于 源码宏 ----------------------
#
# PULSE_HRTIM_CLK_HZ 是固件**全部**时间换算的基准 (分频选档 / 死区 / SYNC 脉宽 /
# PRF 猝发 / 多脉冲补偿), 它必须等于 HRTIM 的实际时钟。G4 的 HRTIM 时钟源不可选
# (HAL 里没有 RCC_HRTIM1CLK_* 选项), 它跟着 APB2 定时器时钟走; .ioc 里的
# RCC.APB2TimFreq_Value 就是 CubeMX 按时钟树算好的那个值。
#
# 为什么必须是 FATAL: 两者不一致时**编译通过、运行正常**, 只是所有输出时间按
# 两者之比整体偏移 —— 属"改了不报错"那一类, 只能靠示波器发现。参见 PORTING.md §3.1。
require_equal_values(
    "G474PulseGen.ioc"
    "RCC.APB2TimFreq_Value=([0-9]+)"
    ".ioc 的 APB2 定时器时钟 (= CK_HRTIM)"
    "Core/Inc/Pulse.h"
    "#define[ \t]+PULSE_HRTIM_CLK_HZ[ \t]+([0-9]+)UL"
    "Pulse.h 的 PULSE_HRTIM_CLK_HZ"
    "改时钟树或改 APB2 分频后, 必须同步修改 Core/Inc/Pulse.h 的 PULSE_HRTIM_CLK_HZ。两者不等时编译不会报错, 但所有输出时间会按两者之比整体偏移。")

message(STATUS "--- ADVISORY: .ioc 侧待办 (不阻断构建) ---")

# USART3 必须在 .ioc 中 —— 这是治本项
advise_match("G474PulseGen.ioc"
    "\nUSART3."
    ".ioc 中已配置 USART3 外设"
    "USART3 不在 .ioc 里时, 每次重新生成都会拆掉它 (构建失败)。请在 CubeMX 中添加: PB10=TX / PB11=RX / AF7 / 2Mbps / 8N1, 并开启 DMA (USART3_TX -> DMA1_Channel2)。")

# SysTick 优先级必须在 .ioc 中同步为 0
advise_match("G474PulseGen.ioc"
    "NVIC.SysTick_IRQn=true.:0.:"
    ".ioc 中 SysTick 优先级 = 0"
    ".ioc 里仍是 15, 与 hal_conf.h 中手工改的 0 不一致 —— 重新生成会把代码改回 15, 静默恢复并发问题。请在 CubeMX 的 NVIC 设置里改为 0。")

# USART3 / DMA 两个中断必须在 .ioc 中使能
advise_match("G474PulseGen.ioc"
    "NVIC.USART3_IRQn=true"
    ".ioc 中使能了 USART3 全局中断"
    "未使能则 CubeMX 不会在 stm32g4xx_it.c 生成 extern huart3 与中断使能代码。")

advise_match("G474PulseGen.ioc"
    "NVIC.DMA1_Channel2_IRQn=true"
    ".ioc 中使能了 DMA1_Channel2 全局中断"
    "USART3_TX 的 DMA 完成中断, 未使能则发送链不工作。")

# .ioc 侧不得残留 Overrun Disable / DMA Disable on RX error
# 用 advise_not_match 而非 require_not_match: 若代码侧已是干净的、只是 .ioc 还勾着,
# 当前构建其实没问题 (要下次重新生成才会出问题), 此时只提示即可。
#
# ⚠ 注意 CubeMX 的命名陷阱: 这两个参数是**永远写出、用值区分开关**的:
#     USART3.OverrunDisableParam=ADVFEATURE_OVERRUN_ENABLE          <- 好 (溢出检测开启)
#     USART3.OverrunDisableParam=ADVFEATURE_OVERRUN_DISABLE         <- 坏
#   即参数名里带 "Disable" 但值为 _ENABLE 才表示该禁用特性未被启用。
#   所以这里匹配的是**坏值**, 不能只匹配参数名 (否则永远误报)。
advise_not_match("G474PulseGen.ioc"
    "USART3.OverrunDisableParam=ADVFEATURE_OVERRUN_DISABLE"
    ".ioc 中未开启 USART3 Overrun Disable"
    "与上一条配套: .ioc 里是 _DISABLE, 重新生成就会再次打开 OVRDIS。请在 CubeMX 的 USART3 -> Advanced Features 中取消勾选。")

advise_not_match("G474PulseGen.ioc"
    "USART3.DMADisableonRxErrorParam=ADVFEATURE_DMA_DISABLEONRXERROR"
    ".ioc 中未开启 DMA Disable on RX error"
    "本工程接收不走 DMA, 该项无实际作用, 但保持与 Overrun Disable 一致的关闭状态以免混淆。")

message(STATUS "")

if(_failed)
    message(STATUS "结果: **检查未通过** —— 上面的 FATAL 项说明重新生成破坏了工程。")
    message(STATUS "      请对照 git diff 恢复被改动的文件, 或参考方案文档的批次 9。")
    message(FATAL_ERROR "CubeMX 重新生成不变量检查未通过")
elseif(_advisory)
    message(STATUS "结果: FATAL 项全部通过; 仍有 .ioc 待办 (见上), 不阻断构建。")
else()
    message(STATUS "结果: 全部通过。")
endif()
