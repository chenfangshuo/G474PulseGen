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

/**
 * @brief  按键扫描与状态机节拍 (由 TIM7 中断每 10ms 调用一次)
 *
 * @note   时基: TIM7 在 tim.c 中被覆写为 PSC=169 / ARR=9999, 即
 *         170MHz ÷ (169+1) ÷ 10000 = 100Hz → **10ms/拍**。
 *         故本文件顶部的时间常量单位都是"拍":
 *           KEY_TIME_LONG   = 50 → 500ms (长按判定阈值)
 *           KEY_TIME_REPEAT = 30 → 300ms (长按连发重复间隔)
 *           KEY_TIME_DOUBLE = 0  → 松开后立即判定为单击 (双击窗口为 0 拍)
 *
 * @note   状态机 S[i] 的五个取值 (裸魔数, 建议后续换成 enum):
 *           S=0 空闲          —— 等待按下
 *           S=1 按下确认中    —— 按下后计时; 若中途松开转 S=2; 若超时转 S=4 并置 KEY_LONG
 *           S=2 等待双击      —— 已松开, 等待二次按下; 二次按下则置 KEY_DOUBLE 转 S=3;
 *                                超时则置 KEY_SINGLE 回 S=0
 *           S=3 双击已完成    —— 等待松开后回 S=0
 *           S=4 长按连发中    —— 每超时一次置 KEY_REPEAT (保持 S=4); 松开回 S=0
 *
 * @warning 本函数运行在 **TIM7 中断上下文**, 内部不得加入阻塞调用或耗时操作。
 *          它写入的 Key_Flag[] 由主循环 Key_Check() 读取 —— Key_Flag 必须保持
 *          volatile, 且当前为文件级 static。
 */
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
