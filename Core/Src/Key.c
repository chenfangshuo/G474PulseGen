/*
 * 本文件改编自 B 站「江协科技」的开源 STM32 教学库, 在此致谢原作者的无私分享。
 *     官方网站: https://jiangxiekeji.com/
 *     原作品公开发布供下载, 但未声明明确的开源许可, 此处保留署名以尊重原作者。
 *
 * 本项目作者编写与修改的部分, 按 MIT 许可证发布, 详见根目录 LICENSE 文件。
 */
#include "stm32g4xx.h"                  // Device header
#include "Key.h"

#define KEY_PRESSED				1
#define KEY_UNPRESSED			0

#define KEY_TIME_DOUBLE			0
#define KEY_TIME_LONG			50
#define KEY_TIME_REPEAT			30

/* 按键状态位图: 由 TIM7 ISR (Key_Tick) 写入, 主循环 Key_Check 读取,
 * 同一编译单元内 static volatile 语义不变, 不可去掉 volatile */
static volatile uint8_t Key_Flag[KEY_COUNT];

void Key_Init(void)
{
}

uint8_t Key_GetState(uint8_t n)
{
	switch (n)
	{
		case K_UP:    return !(GPIOC->IDR & GPIO_PIN_13);
		case K_DOWN:  return !(GPIOA->IDR & GPIO_PIN_4);
		case K_LEFT:  return !(GPIOA->IDR & GPIO_PIN_5);
		case K_RIGHT: return !(GPIOA->IDR & GPIO_PIN_6);
		case K_PRESS: return !(GPIOA->IDR & GPIO_PIN_7);
		case K_ENC:   return !(GPIOB->IDR & GPIO_PIN_0);
		case K_TRG:   return !(GPIOB->IDR & GPIO_PIN_1);
		default:      return KEY_UNPRESSED;
	}
}

uint8_t Key_Check(uint8_t n, uint8_t Flag)
{
	uint8_t ret = 0;
	uint32_t pm = __get_PRIMASK(); __disable_irq();
	if (Key_Flag[n] & Flag) {
		if (Flag != KEY_HOLD) Key_Flag[n] &= (uint8_t)~Flag;
		ret = 1;
	}
	__set_PRIMASK(pm);
	return ret;
}

void Key_Tick(void)
{
	static uint8_t i;
	static uint8_t CurrState[KEY_COUNT], PrevState[KEY_COUNT];
	static uint8_t S[KEY_COUNT];
	static uint16_t Time[KEY_COUNT];

	/* 递减计时器 */
	for (i = 0; i < KEY_COUNT; i ++)
	{
		if (Time[i] > 0)
		{
			Time[i] --;
		}
	}

	/* 每次进入中断直接进行按键扫描与状态机判定 (10ms 一次) */
	for (i = 0; i < KEY_COUNT; i ++)
	{
		PrevState[i] = CurrState[i];
		CurrState[i] = Key_GetState(i);

		if (CurrState[i] == KEY_PRESSED)
		{
			Key_Flag[i] |= KEY_HOLD;
		}
		else
		{
			Key_Flag[i] &= ~KEY_HOLD;
		}

		if (CurrState[i] == KEY_PRESSED && PrevState[i] == KEY_UNPRESSED)
		{
			Key_Flag[i] |= KEY_DOWN;
		}

		if (CurrState[i] == KEY_UNPRESSED && PrevState[i] == KEY_PRESSED)
		{
			Key_Flag[i] |= KEY_UP;
		}

		if (S[i] == 0)
		{
			if (CurrState[i] == KEY_PRESSED)
			{
				Time[i] = KEY_TIME_LONG;
				S[i] = 1;
			}
		}
		else if (S[i] == 1)
		{
			if (CurrState[i] == KEY_UNPRESSED)
			{
				Time[i] = KEY_TIME_DOUBLE;
				S[i] = 2;
			}
			else if (Time[i] == 0)
			{
				Time[i] = KEY_TIME_REPEAT;
				Key_Flag[i] |= KEY_LONG;
				S[i] = 4;
			}
		}
		else if (S[i] == 2)
		{
			if (CurrState[i] == KEY_PRESSED)
			{
				Key_Flag[i] |= KEY_DOUBLE;
				S[i] = 3;
			}
			else if (Time[i] == 0)
			{
				Key_Flag[i] |= KEY_SINGLE;
				S[i] = 0;
			}
		}
		else if (S[i] == 3)
		{
			if (CurrState[i] == KEY_UNPRESSED)
			{
				S[i] = 0;
			}
		}
		else if (S[i] == 4)
		{
			if (CurrState[i] == KEY_UNPRESSED)
			{
				S[i] = 0;
			}
			else if (Time[i] == 0)
			{
				Time[i] = KEY_TIME_REPEAT;
				Key_Flag[i] |= KEY_REPEAT;
				S[i] = 4;
			}
		}
	}
}
