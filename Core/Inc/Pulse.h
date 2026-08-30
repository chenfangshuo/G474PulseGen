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
#define PULSE_MODE_NPULSE               1   /* N 脉冲 (HRTIM 短脉冲) */
#define PULSE_MODE_DPULSE               2
#define PULSE_MODE_PWM                  3
#define PULSE_MODE_NONE                 4
#define PULSE_MODE_NPULSE_LONG          6   /* N 脉冲 Long (TIM5 长脉冲) */
#define PULSE_MODE_PWM_LONG             7
#define PULSE_MODE_COMP_PWM             8   /* 互补 PWM (HRTIM 高精度, 死区硬件生成) */
#define PULSE_MODE_COMP_PWM_LONG        9   /* 互补 PWM Long (TIM5 超长, 软件死区) */

/* 互补通道对索引 (严格遵循 HARDWARE.md §5.1 输出映射, 同一定时器 Tx1/Tx2) */
#define COMP_PAIR_CH1_CH2               0   /* CH1(TB2,PA11) + CH2(TB1,PA10) -> Timer B */
#define COMP_PAIR_CH3_CH4               1   /* CH3(TA2,PA9)  + CH4(TA1,PA8)  -> Timer A */
#define COMP_PAIR_CH5_CH6               2   /* CH5(TD2,PB15) + CH6(TD1,PB14) -> Timer D */
#define COMP_PAIR_CH7_CH8               3   /* CH7(TC2,PB13) + CH8(TC1,PB12) -> Timer C */

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
    volatile uint8_t          channel;               /* 当前选中的逻辑通道 CH1 ~ CH8 */
    volatile uint8_t          mode;                  /* PULSE_MODE_* */
    volatile uint8_t          timer_idx;             /* HRTIM_TIMERINDEX_TIMER_A/B/C/D */
    volatile uint32_t         timer_id;              /* HRTIM_TIMERID_TIMER_A/B/C/D */
    volatile uint32_t         output_ch;             /* 主路输出 (互补模式为 Tx1 参考路) */
    volatile uint32_t         output_ch2;            /* 互补输出 (互补模式为 Tx2, 单通道模式置 0) */
    volatile uint8_t          pair_idx;              /* 互补通道对索引 COMP_PAIR_* (0~3) */
    volatile uint8_t          polarity;              /* PULSE_POLARITY_HIGH / LOW */
    volatile bool             is_enabled;            /* 输出使能标志 */
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
bool Pulse_nPulse_SetPW(float pw, float interval_us, uint32_t count);
void Pulse_nPulse_Init(void);
void Pulse_nPulse_OnTrigger(uint32_t count);
void Pulse_nPulse_OnPeriodEnd(void);
bool Pulse_dPulse_SetPW(int32_t pw1, int32_t interval, int32_t pw2);
void Pulse_dPulse_Init(void);
bool Pulse_PWM_SetPW(float period_us, int32_t duty_cycle_percent);
void Pulse_PWM_Init(void);
void Pulse_nPulseLong_SetPW(float pw, float interval_s);
void Pulse_nPulseLong_Init(void);
void Pulse_lPWM_SetPW(float period_s, float duty_cycle_percent);
void Pulse_lPWM_Init(void);

/* 互补 PWM (HRTIM 高精度) 与互补 PWM Long (TIM5 超长) 驱动接口 */
void Pulse_Select_CompPair(uint8_t pair_idx);
void Pulse_CompPWM_Init(void);
bool Pulse_CompPWM_SetPW(float period_us, int32_t duty_percent, uint32_t dt_rise_ns, uint32_t dt_fall_ns);
void Pulse_CompLPWM_Init(void);
bool Pulse_CompLPWM_SetPW(float period_s, float duty_percent, uint32_t dt_ms);
void Pulse_CompLPWM_OnPeriodElapsed(void);   /* 周期起点: 互补路关断 */
void Pulse_CompLPWM_OnDuty(void);            /* CC1 匹配(占空比点): 主路关断 */
void Pulse_CompLPWM_OnCompOn(void);          /* CC2 匹配(占空比+死区): 互补路开启 */
void Pulse_CompLPWM_OnMainOn(void);          /* CC3 匹配(死区点): 主路开启 */

/* 长脉冲 (TIM5 软件模式) 引脚电平与中断服务接口 */
void Pulse_LongPin_SetActive(void);
void Pulse_LongPin_SetInactive(void);
void Pulse_nPulseLong_OnTrigger(uint32_t count);
void Pulse_nPulseLong_OnPeriodElapsed(void);
void Pulse_nPulseLong_OnCompareMatch(void);

/* 硬件级与软件级紧急快速关断 (Safe-State) */
void Pulse_EmergencyStop(void);

GPIO_TypeDef *Pulse_GetLongPulsePort(void);
uint16_t Pulse_GetLongPulsePin(void);

#endif /* __PULSE_H */