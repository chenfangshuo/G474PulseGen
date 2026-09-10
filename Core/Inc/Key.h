/*
 * 本文件改编自 B 站「江协科技」的开源 STM32 教学库, 在此致谢原作者的无私分享。
 *     官方网站: https://jiangxiekeji.com/
 *     原作品公开发布供下载, 但未声明明确的开源许可, 此处保留署名以尊重原作者。
 *
 * 本项目作者编写与修改的部分, 按 MIT 许可证发布, 详见根目录 LICENSE 文件。
 */
#ifndef __KEY_H
#define __KEY_H

#include "main.h"

#define KEY_COUNT				7

#define K_UP					0
#define K_DOWN			    	1
#define K_LEFT			    	2
#define K_RIGHT			    	3
#define K_PRESS			    	4
#define K_ENC				    5
#define K_TRG				    6

/* 硬件消抖 MAX6818 输出引脚语义宏映射 (严格对应 HARDWARE.md §3.1) */
#define KEY_UP_GPIO_Port        GPIOC
#define KEY_UP_Pin              GPIO_PIN_13

#define KEY_DOWN_GPIO_Port      GPIOA
#define KEY_DOWN_Pin            GPIO_PIN_4

#define KEY_LEFT_GPIO_Port      GPIOA
#define KEY_LEFT_Pin            GPIO_PIN_5

#define KEY_RIGHT_GPIO_Port     GPIOA
#define KEY_RIGHT_Pin           GPIO_PIN_6

#define KEY_CENTER_GPIO_Port    GPIOA
#define KEY_CENTER_Pin          GPIO_PIN_7

#define KEY_ENC_GPIO_Port       GPIOB
#define KEY_ENC_Pin             GPIO_PIN_0

#define KEY_TRG_GPIO_Port       GPIOB
#define KEY_TRG_Pin             GPIO_PIN_1

#define KEY_HOLD				0x01
#define KEY_DOWN				0x02
#define KEY_UP					0x04
#define KEY_SINGLE				0x08
#define KEY_DOUBLE				0x10
#define KEY_LONG				0x20
#define KEY_REPEAT				0x40

void Key_Init(void);
uint8_t Key_Check(uint8_t n, uint8_t Flag);
void Key_Tick(void);

#endif
