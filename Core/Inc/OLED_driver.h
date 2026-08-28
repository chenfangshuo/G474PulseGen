#ifndef __OLED_DRIVER_H
#define __OLED_DRIVER_H



#include "stm32g4xx.h"
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include "stdio.h"
//使用宏定义，速度更快（寄存器方式）
// #define OLED_SCL_Clr()  (GPIOB->BRR = GPIO_PIN_3)   // 复位 SCL (将 GPIOB 的 3 号引脚拉低)
// #define OLED_SCL_Set()  (GPIOB->BSRR = GPIO_PIN_3)  // 置位 SCL (将 GPIOB 的 3 号引脚拉高)
//
// #define OLED_SDA_Clr()  (GPIOB->BRR = GPIO_PIN_5)   // 复位 SDA (将 GPIOB 的 5 号引脚拉低)
// #define OLED_SDA_Set()  (GPIOB->BSRR = GPIO_PIN_5)  // 置位 SDA (将 GPIOB 的 5 号引脚拉高)

#define OLED_RES_Clr()  (GPIOB->BRR = GPIO_PIN_7)   // 复位 RES (将 GPIOB 的 7 号引脚拉低)
#define OLED_RES_Set()  (GPIOB->BSRR = GPIO_PIN_7)  // 置位 RES (将 GPIOB 的 7 号引脚拉高)

#define OLED_DC_Clr()   (GPIOB->BRR = GPIO_PIN_6)   // 复位 DC (将 GPIOB 的 6 号引脚拉低)
#define OLED_DC_Set()   (GPIOB->BSRR = GPIO_PIN_6)  // 置位 DC (将 GPIOB 的 6 号引脚拉高)

#define OLED_CS_Clr()   (GPIOB->BRR = GPIO_PIN_4)   // 复位 CS (将 GPIOB 的 4 号引脚拉低)
#define OLED_CS_Set()   (GPIOB->BSRR = GPIO_PIN_4)  // 置位 CS (将 GPIOB 的 4 号引脚拉高)



#define OLED_CMD  0	//写命令
#define OLED_DATA 1	//写数据


//	oled初始化函数
void OLED_Init(void);

void OLED_Update_DisplayBuf(uint8_t DisplayBuf[128/8][128]);
//	oled全局刷新函数
void OLED_Update(void);
//	oled局部刷新函数
void OLED_UpdateArea(uint8_t X, uint8_t Y, uint8_t Width, uint8_t Height);
// 设置颜色模式
void OLED_SetColorMode(bool colormode);
// OLED 设置亮度函数
void OLED_Brightness(int16_t Brightness);





#endif







