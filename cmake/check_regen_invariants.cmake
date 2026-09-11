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
