#ifndef __PULSE_H
#define __PULSE_H

#include "main.h"
#include <stdbool.h>

/* 输出通道定义 (对齐 HARDWARE.md §5.1 输出映射) */
#define CH_NONE                         0
#define CH1                             1   /* HRTIM1_CHB2 (PA11 -> Y1) */
#define CH2                             2   /* HRTIM1_CHB1 (PA10 -> Y2) */
#define CH3                             3   /* HRTIM1_CHA2 (PA9  -> Y3) */
#define CH4                             4   /* HRTIM1_CHA1 (PA8  -> Y4) */
#define CH5                             5   /* HRTIM1_CHD2 (PB15 -> Y5) */
#define CH6                             6   /* HRTIM1_CHD1 (PB14 -> Y6) */
#define CH7                             7   /* HRTIM1_CHC2 (PB13 -> Y7) */
#define CH8                             8   /* HRTIM1_CHC1 (PB12 -> Y8) */

/* 发波模式定义 */
#define PULSE_MODE_SINGLE               0
#define PULSE_MODE_NPULSE               1
#define PULSE_MODE_DPULSE               2
#define PULSE_MODE_PWM                  3
#define PULSE_MODE_NONE                 4
#define PULSE_MODE_SINGLE_LONG          5
#define PULSE_MODE_NPULSE_LONG          6
#define PULSE_MODE_PWM_LONG             7

/* 脉冲极性定义 */
#define PULSE_POLARITY_HIGH             0
#define PULSE_POLARITY_LOW              1

#define US_TO_S(us)         ((float)(us) / 1000000.0f)

/* HRTIM 通道 GPIO 硬件语义宏 (严格遵循 HARDWARE.md §5.1) */
#define HRT_CHA1_GPIO_Port              GPIOA
#define HRT_CHA1_Pin                    GPIO_PIN_8
#define HRT_CHA2_GPIO_Port              GPIOA
#define HRT_CHA2_Pin                    GPIO_PIN_9

#define HRT_CHB1_GPIO_Port              GPIOA
#define HRT_CHB1_Pin                    GPIO_PIN_10
#define HRT_CHB2_GPIO_Port              GPIOA
#define HRT_CHB2_Pin                    GPIO_PIN_11

#define HRT_CHC1_GPIO_Port              GPIOB
#define HRT_CHC1_Pin                    GPIO_PIN_12
#define HRT_CHC2_GPIO_Port              GPIOB
#define HRT_CHC2_Pin                    GPIO_PIN_13

#define HRT_CHD1_GPIO_Port              GPIOB
#define HRT_CHD1_Pin                    GPIO_PIN_14
#define HRT_CHD2_GPIO_Port              GPIOB
#define HRT_CHD2_Pin                    GPIO_PIN_15

/* Pulse 控制器上下文结构体 (重构全局状态机，消除类型溢出隐患) */
typedef struct {
    uint8_t   channel;          /* 当前选中的逻辑通道 CH1 ~ CH8 */
    uint8_t   mode;             /* PULSE_MODE_* */
    uint8_t   timer_idx;        /* HRTIM_TIMERINDEX_TIMER_A/B/C/D */
    uint32_t  timer_id;         /* HRTIM_TIMERID_TIMER_A/B/C/D */
    uint32_t  output_ch;        /* HRTIM_OUTPUT_TA1/TA2/TB1/TB2/TC1/TC2/TD1/TD2 */
    uint8_t   polarity;         /* PULSE_POLARITY_HIGH / LOW */
    bool      is_enabled;       /* 输出使能标志 */
} Pulse_Controller_t;

extern Pulse_Controller_t g_pulse_ctrl;

/* 兼容性全局变量声明 (兼容既有 UI / main.c 模块) */
extern volatile uint32_t HRTIM_TIMERINDEX_TIMER_X;
extern volatile uint32_t HRTIM_TIMERID_TIMER_X;
extern volatile uint32_t HRTIM_OUTPUT_TXX;
extern volatile uint8_t  PULSE_MODE;
extern volatile bool     PULSE_OUT_ENABLED;
extern volatile bool     PULSE_POLARITY;
extern volatile uint32_t lpwm_arr;
extern volatile uint32_t lpwm_ccr;

/* 驱动接口函数 */
void Pulse_Select_Output(uint8_t CHx);
void Pulse_Enable_Output(void);
void Pulse_Disable_Output(void);
void Pulse_SetPulsePolarity_High(void);
void Pulse_SetPulsePolarity_Low(void);
bool Pulse_nPulse_SetPW(float pw);
void Pulse_nPulse_Init(void);
bool Pulse_dPulse_SetPW(int32_t pw1, int32_t interval, int32_t pw2);
void Pulse_dPulse_Init(void);
bool Pulse_PWM_SetPW(float period_us, int32_t duty_cycle_percent);
void Pulse_PWM_Init(void);
void Pulse_slPulse_SetPW(float pw);
void Pulse_slPulse_Init(void);
void Pulse_lPWM_SetPW(float period_s, float duty_cycle_percent);
void Pulse_lPWM_Init(void);

GPIO_TypeDef *Pulse_GetLongPulsePort(void);
uint16_t Pulse_GetLongPulsePin(void);

#endif /* __PULSE_H */