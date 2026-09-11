cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME G474PulseGen)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME}
        Core/Src/OLED.c
        Core/Src/OLED_driver.c
        Core/Src/OLED_Fonts.c
        Core/Src/WouoUI.c
        Core/Src/WouoUI_anim.c
        Core/Src/WouoUI_font.c
        Core/Src/WouoUI_graph.c
        Core/Src/WouoUI_msg.c
        Core/Src/WouoUI_page.c
        Core/Src/WouoUI_user.c
        Core/Src/WouoUI_win.c
        Core/Src/Key.c
        Core/Src/Pulse.c
        Core/Src/Preset.c
        Core/Src/uart_comm.c)

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# ---------------------------------------------------------------------------
# 第三方代码告警隔离 (配合 -Wall -Wextra)
# 目的: 让 HAL/CMSIS 头与 ST 生成的桩文件不产生告警, 把告警面留给自有代码。
# 注意: 这些设置放在根 CMakeLists 而非 cmake/stm32cubemx/ 下, 因为后者是
#       CubeMX 会重新生成的文件。
# ---------------------------------------------------------------------------

# 1) HAL/CMSIS 头文件目录标记为 SYSTEM, 走 -isystem 而不产生第三方告警。
#    自有代码目录 Core/Inc 保持非 SYSTEM, 其告警仍然可见。
target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Inc/Legacy
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32G4xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include
)

# 2) ST HAL 驱动源文件整体降级 (STM32_Drivers OBJECT 库含 HAL 与
#    system_stm32g4xx.c)。它们是 ST 官方代码, 不在本项目维护范围内。
if(TARGET STM32_Drivers)
    target_compile_options(STM32_Drivers PRIVATE -w)
endif()

# 3) CubeMX/ST 模板桩文件逐个降级: syscalls.c / sysmem.c 是 newlib 桩,
#    其保留标识符与签名由 libc 约定决定, 不可改名也不应产生告警。
set_source_files_properties(
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

# 4) 第三方移植库文件: 只抑制三类**经人工逐条确认非缺陷**的告警, 其余保持可见。
#    来源为 WouoUI-PageVersion (MPL-2.0) 与江协科技教学库, 不在本项目维护范围。
#    -Wno-unused-parameter      框架回调签名固定, 参数未用属正常
#    -Wno-empty-body            断言/日志宏关闭时展开为空语句 (已核对, 非缺陷)
#    -Wno-implicit-fallthrough  switch 有意贯穿 (已核对, 但见 README 待确认项)
#    注意: 自有代码 WouoUI_user.c 与 Key.c **刻意不在此列**, 其告警保持可见。
set_source_files_properties(
    Core/Src/WouoUI.c
    Core/Src/WouoUI_anim.c
    Core/Src/WouoUI_font.c
    Core/Src/WouoUI_graph.c
    Core/Src/WouoUI_msg.c
    Core/Src/WouoUI_page.c
    Core/Src/WouoUI_win.c
    Core/Src/OLED.c
    Core/Src/OLED_Fonts.c
    Core/Src/OLED_driver.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-empty-body;-Wno-implicit-fallthrough"
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
# 用户源文件统一在上方 add_executable() 中声明。
# 原此处有一份与之完全重复的 15 文件清单，改一处漏一处易出错，已合并删除。
# 新增用户源文件请直接加到 add_executable() 的列表中。

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    Core/Inc
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# ---------------------------------------------------------------------------
# CubeMX 重新生成防线
#
# 每次构建前校验若干"游离于 .ioc 之外"的关键配置是否被重新生成破坏。
# 背景: 2026-09 的一次重新生成把 TICK_INT_PRIORITY 从 0 静默改回 15, 恢复了
#       中断优先级反转的前提条件 —— 编译通过、运行正常, 极难发现。
#       详见 cmake/check_regen_invariants.cmake 的说明。
#
# 分级: FATAL 项会令本目标失败, 从而中断整个构建 (这是刻意的);
#       .ioc 侧的 ADVISORY 项只打印提示, 不阻断。
# 手动运行: cmake -P cmake/check_regen_invariants.cmake
# ---------------------------------------------------------------------------
add_custom_target(check_regen_invariants ALL
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_regen_invariants.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "校验 CubeMX 重新生成不变量"
    VERBATIM
)
add_dependencies(${CMAKE_PROJECT_NAME} check_regen_invariants)
