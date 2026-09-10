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

// oled初始化函数
void OLED_Init(void);

void OLED_Update_DisplayBuf(uint8_t DisplayBuf[128/8][128]);
// oled全局刷新函数
void OLED_Update(void);
// oled局部刷新函数
void OLED_UpdateArea(uint8_t X, uint8_t Y, uint8_t Width, uint8_t Height);
// 设置颜色模式
void OLED_SetColorMode(bool colormode);
// OLED 设置亮度函数
void OLED_Brightness(int16_t Brightness);

// SPI DMA 发送中断回调
void OLED_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi);

#endif
