#include "stm32g4xx.h"                  // Device header
#include "Key.h"

#define KEY_PRESSED				1
#define KEY_UNPRESSED			0

#define KEY_TIME_DOUBLE			0
#define KEY_TIME_LONG			500
#define KEY_TIME_REPEAT			300

volatile uint8_t Key_Flag[KEY_COUNT];

void Key_Init(void)
{
}

uint8_t Key_GetState(uint8_t n)
{
	switch (n)
	{
		case K_UP:
			return (HAL_GPIO_ReadPin(KEY_UP_GPIO_Port, KEY_UP_Pin) == GPIO_PIN_RESET) ? KEY_PRESSED : KEY_UNPRESSED;
		case K_DOWN:
			return (HAL_GPIO_ReadPin(KEY_DOWN_GPIO_Port, KEY_DOWN_Pin) == GPIO_PIN_RESET) ? KEY_PRESSED : KEY_UNPRESSED;
		case K_LEFT:
			return (HAL_GPIO_ReadPin(KEY_LEFT_GPIO_Port, KEY_LEFT_Pin) == GPIO_PIN_RESET) ? KEY_PRESSED : KEY_UNPRESSED;
		case K_RIGHT:
			return (HAL_GPIO_ReadPin(KEY_RIGHT_GPIO_Port, KEY_RIGHT_Pin) == GPIO_PIN_RESET) ? KEY_PRESSED : KEY_UNPRESSED;
		case K_PRESS:
			return (HAL_GPIO_ReadPin(KEY_CENTER_GPIO_Port, KEY_CENTER_Pin) == GPIO_PIN_RESET) ? KEY_PRESSED : KEY_UNPRESSED;
		case K_ENC:
			return (HAL_GPIO_ReadPin(KEY_ENC_GPIO_Port, KEY_ENC_Pin) == GPIO_PIN_RESET) ? KEY_PRESSED : KEY_UNPRESSED;
		case K_TRG:
			return (HAL_GPIO_ReadPin(KEY_TRG_GPIO_Port, KEY_TRG_Pin) == GPIO_PIN_RESET) ? KEY_PRESSED : KEY_UNPRESSED;
		default:
			return KEY_UNPRESSED;
	}
}

uint8_t Key_Check(uint8_t n, uint8_t Flag)
{
	if (Key_Flag[n] & Flag)
	{
		if (Flag != KEY_HOLD)
		{
			Key_Flag[n] &= ~Flag;
		}
		return 1;
	}
	return 0;
}

void Key_Tick(void)
{
	static uint8_t Count, i;
	static uint8_t CurrState[KEY_COUNT], PrevState[KEY_COUNT];
	static uint8_t S[KEY_COUNT];
	static uint16_t Time[KEY_COUNT];
	
	for (i = 0; i < KEY_COUNT; i ++)
	{
		if (Time[i] > 0)
		{
			Time[i] --;
		}
	}
	
	Count ++;
	if (Count >= 20)
	{
		Count = 0;
		
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
}
