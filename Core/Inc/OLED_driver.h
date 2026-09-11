/*
 * 本文件改编自 B 站「江协科技」的开源 STM32 教学库, 在此致谢原作者的无私分享。
 *     官方网站: https://jiangxiekeji.com/
 *     原作品公开发布供下载, 但未声明明确的开源许可, 此处保留署名以尊重原作者。
 *
 * 本项目作者编写与修改的部分, 按 MIT 许可证发布, 详见根目录 LICENSE 文件。
 */
#ifndef __OLED_DRIVER_H
#define __OLED_DRIVER_H

#include "stm32g4xx.h"
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include "stdio.h"

#define OLED_RES_Clr()  (GPIOB->BRR = GPIO_PIN_7)   // 复位 RES (将 GPIOB 的 7 号引脚拉低)
#define OLED_RES_Set()  (GPIOB->BSRR = GPIO_PIN_7)  // 置位 RES (将 GPIOB 的 7 号引脚拉高)

#define OLED_DC_Clr()   (GPIOB->BRR = GPIO_PIN_6)   // 复位 DC (将 GPIOB 的 6 号引脚拉低)
#define OLED_DC_Set()   (GPIOB->BSRR = GPIO_PIN_6)  // 置位 DC (将 GPIOB 的 6 号引脚拉高)

#define OLED_CS_Clr()   (GPIOB->BRR = GPIO_PIN_4)   // 复位 CS (将 GPIOB 的 4 号引脚拉低)
#define OLED_CS_Set()   (GPIOB->BSRR = GPIO_PIN_4)  // 置位 CS (将 GPIOB 的 4 号引脚拉高)

#define OLED_CMD  0	//写命令
#define OLED_DATA 1	//写数据

/* OLED 显存 (定义于 OLED_driver.c)。
 * 维度表达式与定义侧完全一致 (128/8 = 16 页 × 128 列 = 2048 字节)。
 * 此前 uart_comm.c 手写了一份 extern 声明为 [16][128] —— 数值恰好相同,
 * 但两处独立书写, 一旦屏尺寸调整就会变成静默的缓冲区越界, 故统一到此处。
 * 注: 不用 OLED_WIDTH/OLED_HEIGHT 表达是因为它们定义在 OLED.h, 而 OLED.h
 * 反过来包含了本文件, 引入会形成头文件循环。 */
extern uint8_t OLED_DisplayBuf[128/8][128];

// oled初始化函数
void OLED_Init(void);

void OLED_Update_DisplayBuf(uint8_t DisplayBuf[128/8][128]);
// oled全局刷新函数
void OLED_Update(void);
/* 已删除: void OLED_UpdateArea(uint8_t X, uint8_t Y, uint8_t Width, uint8_t Height)
 *
 * 原为江协库的"局部刷新"API, 但函数体只有一句 OLED_Update() —— 四个形参
 * X/Y/Width/Height 全部未使用, 实为全屏刷新的空壳, 不提供任何局部刷新能力。
 * 2026-09 核查确认全工程无任何调用点后删除。
 *
 * 保留此记录的缘由: 该函数的签名会让人误以为本库支持局部刷新, 删掉它同时
 * 也去掉了 OLED_driver.c 唯一的 -Wunused-parameter 告警源 (4 条), 使该文件
 * 得以从 CMakeLists.txt 的第三方告警抑制名单中移出。
 *
 * 若将来确需局部刷新, 需实现带窗口设置的 SSD1306 命令序列
 * (0x21 列地址 / 0x22 页地址), 不能靠"调全屏刷新"来冒充。 */
// 设置颜色模式
void OLED_SetColorMode(bool colormode);
// OLED 设置亮度函数
void OLED_Brightness(int16_t Brightness);

// SPI DMA 发送中断回调
void OLED_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi);

#endif
