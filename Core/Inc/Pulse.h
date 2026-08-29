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
#define PULSE_MODE_INTERLEAVED_PWM      8   /* 180° 交错并联 PWM 模式 */

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

/* 软启动状态机枚举 */
typedef enum {
    SOFTSTART_STATE_IDLE = 0,       /* 空闲状态 */
    SOFTSTART_STATE_RAMPING,        /* 占空比平滑爬升中 */
    SOFTSTART_STATE_RUNNING,        /* 达到目标占空比稳定运行 */
    SOFTSTART_STATE_STOPPING,       /* 软关断斜坡下降中 */
    SOFTSTART_STATE_FAULT           /* 故障跳闸关断状态 */
} SoftStart_State_t;

/* 软启动控制参数结构体 */
typedef struct {
    SoftStart_State_t state;        /* 软启动状态机阶段 */
    float             current_duty; /* 当前实时占空比 (0.0 ~ 100.0%) */
    float             target_duty;  /* 目标设定占空比 (0.0 ~ 100.0%) */
    float             step_duty;    /* 每 10ms Tick 步进占空比增量 */
    float             period_us;    /* 当前 PWM 周期 (us) */
    bool              auto_loadsw;  /* 软启动完成后是否自动使能 LOADSW */
} SoftStart_Ctrl_t;

/* Pulse 控制器上下文结构体 (重构全局状态机，消除类型溢出隐患) */
typedef struct {
    volatile uint8_t          channel;               /* 当前选中的逻辑通道 CH1 ~ CH8 */
    volatile uint8_t          mode;                  /* PULSE_MODE_* */
    volatile uint8_t          timer_idx;             /* HRTIM_TIMERINDEX_TIMER_A/B/C/D */
    volatile uint32_t         timer_id;              /* HRTIM_TIMERID_TIMER_A/B/C/D */
    volatile uint32_t         output_ch;             /* HRTIM_OUTPUT_TA1/TA2/TB1/TB2/TC1/TC2/TD1/TD2 */
    volatile uint8_t          polarity;              /* PULSE_POLARITY_HIGH / LOW */
    volatile bool             is_enabled;            /* 输出使能标志 */
    volatile uint16_t         deadtime_rising_val;   /* 死区上升沿计数值 (0=禁用死区) */
    volatile uint16_t         deadtime_falling_val;  /* 死区下降沿计数值 (0=禁用死区) */
    volatile SoftStart_Ctrl_t softstart;             /* 软启动控制器 */
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

/* Step 3: 死区发生器配置接口 (防上下桥直通短路) */
bool Pulse_SetDeadTime(uint8_t timer_idx, uint16_t rising_ns, uint16_t falling_ns);
void Pulse_DisableDeadTime(uint8_t timer_idx);

/* Step 4: 180° 交错 PWM 与软启动控制接口 */
void Pulse_InterleavedPWM_Init(float period_us, float initial_duty);
bool Pulse_InterleavedPWM_SetPW(float period_us, float duty_percent);
void Pulse_SoftStart_Start(float target_duty, uint32_t ramp_time_ms, bool enable_loadsw);
void Pulse_SoftStart_Stop(uint32_t ramp_time_ms);
void Pulse_SoftStart_Update(void);

/* Step 5: 硬件级与软件级紧急快速关断 (Safe-State) */
void Pulse_EmergencyStop(void);

GPIO_TypeDef *Pulse_GetLongPulsePort(void);
uint16_t Pulse_GetLongPulsePin(void);

#endif /* __PULSE_H */