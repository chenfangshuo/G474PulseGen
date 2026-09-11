#ifndef PULSE_H
#define PULSE_H

#include "main.h"
#include <stdbool.h>

/* ===================== 公共 API 的单位约定 =====================
 * 本模块的 float 参数一律使用**物理单位**, 由调用方负责缩放:
 *   - 时间: 短脉冲/HRTIM 路径用 **µs**; 超长路径用 **s**;
 *           死区用 **ns** (HRTIM 硬件死区) 或 **ms** (TIM5 软件死区)
 *   - 占空比: **百分数** (0~100 或 0.01~100), 不是 0~1 的小数
 *
 * UI 与 SCPI 侧传参前会做定点缩放, 而该缩放目前**分散在三处独立书写**:
 *   WouoUI_user.c (UI 旋钮)   /100.0f、/1000.0f
 *   uart_comm.c   (SCPI 文本) 同上
 *   Preset.c      (Flash 存取) 同上
 * 三处必须保持一致 —— 改动任一处的放大倍率都要同步其余两处, 否则会出现
 * "屏幕显示正常但 SCPI 下发值差 10 倍"这类很难定位的问题。
 * ============================================================= */

/* 输出通道定义 (对齐 HARDWARE.md §5.1 输出映射) */
#define CH_NONE                         0
#define CH1                             1   /* HRTIM1_CHB2 (PA11 -> Y1) */
#define CH2                             2   /* HRTIM1_CHB1 (PA10 -> Y2) */
#define CH3                             3   /* HRTIM1_CHA2 (PA9  -> Y3) */
#define CH4                             4   /* HRTIM1_CHA1 (PA8  -> Y4) */
#define CH5                             5   /* HRTIM1_CHD2 (PB15 -> Y5) */
#define CH6                             6   /* HRTIM1_CHD1 (PB14 -> Y6) */

/* Y7/Y8 专用功能引脚 (Step 0 起 Timer C 剥离发波) */
#define SYNC_OUT_GPIO_Port              GPIOB
#define SYNC_OUT_Pin                    GPIO_PIN_13   /* Y7 = HRTIM CHC2, SYNC OUT */
#define FRAME_OUT_GPIO_Port             GPIOB
#define FRAME_OUT_Pin                   GPIO_PIN_12   /* Y8 = 帧/猝发标记 (软件 GPIO) */
#define SYNC_OUT_WIDTH_TICKS            1088U         /* SYNC 脉宽 200ns: 1088 = 200ns × 5.44GHz, 其中 5.44GHz = 170MHz × 32 (MUL32 档) */
#define BURST_PRF_MAX_HZ                100000U       /* PRF 猝发重复频率上限 */

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

/* 脉冲极性定义 */
#define PULSE_POLARITY_HIGH             0
#define PULSE_POLARITY_LOW              1

/* µs → s: 供 roundf(t_s × f_HRTIM) 做频率换算前消除量纲 */
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
    volatile uint8_t          channel;               /* 当前选中的逻辑通道 CH1 ~ CH6 */
    volatile uint8_t          mode;                  /* PULSE_MODE_* */
    volatile uint8_t          timer_idx;             /* HRTIM_TIMERINDEX_TIMER_A/B/C/D */
    volatile uint32_t         timer_id;              /* HRTIM_TIMERID_TIMER_A/B/C/D */
    volatile uint32_t         output_ch;             /* 主路输出 (互补模式为 Tx1 参考路) */
    volatile uint32_t         output_ch2;            /* 互补输出 (互补模式为 Tx2, 单通道模式置 0) */
    volatile uint8_t          pair_idx;              /* 互补通道对索引 COMP_PAIR_* (0~2) */
    volatile uint8_t          polarity;              /* PULSE_POLARITY_HIGH / LOW */
    volatile bool             is_enabled;            /* 输出使能标志 */
} Pulse_Controller_t;

extern Pulse_Controller_t g_pulse_ctrl;

/* 兼容性全局变量声明 (兼容既有 UI / main.c 模块) */
/* 以下 6 个是**变量, 不是宏** —— 早期版本曾用全大写命名 (PULSE_MODE /
 * HRTIM_TIMERINDEX_TIMER_X 等), 极易被误读为编译期常量, 故统一改为 g_ 前缀。
 * 它们全部 volatile, 并在 ISR 中被异步读写 (见 Pulse_SyncContext 的同步点)。 */
extern volatile uint32_t g_hrtim_timer_index;   /* 当前 HRTIM timer index */
extern volatile uint32_t g_hrtim_timer_id;      /* 当前 HRTIM timer id (MCR 位) */
extern volatile uint32_t g_hrtim_output;        /* 当前输出通道 (HRTIM_OUTPUT_Txx) */
extern volatile uint8_t  g_pulse_mode;          /* 当前发波模式 (PULSE_MODE_*) */
extern volatile bool     g_pulse_out_enabled;   /* 输出使能 */
extern volatile bool     g_pulse_polarity;      /* 脉冲极性 (PULSE_POLARITY_*) */
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

/* ==================== Y7 SYNC OUT / Y8 帧标记 / Burst PRF ==================== */
void Pulse_Sync_Init(void);              /* 配置 Timer C CH2 (Y7) 为单次同步脉冲 */
void Pulse_TriggerFireAll(void);         /* 同一写操作同步复位波形定时器与 SYNC Timer C */
void Pulse_Frame_SetActive(void);        /* 帧标记 (Y8) 拉高: 猝发开始 */
void Pulse_Frame_SetInactive(void);      /* 帧标记 (Y8) 拉低: 猝发结束 */
void Pulse_BurstPRF_Init(void);          /* TIM3 周期猝发时基初始化 */
void Pulse_BurstPRF_Set(uint32_t prf_hz);/* 设置猝发重复频率 (0 = 单次, 1~100000 Hz) */
void Pulse_BurstPRF_Start(void);         /* 启动周期猝发 (立即发第一帧) */
void Pulse_BurstPRF_Stop(void);          /* 停止周期猝发 */
void Pulse_BurstPRF_OnTick(void);        /* TIM3 更新中断: 周期重发一帧猝发 */

/* PA15 = HRTIM_FLT2 硬件故障封锁 (低电平触发, 输出 ns 级强制无效) */
void Pulse_Fault_Init(void);
extern volatile bool g_fault_flag;   /* Fault 发生标志 (ISR 置位, main 循环消费) */

#endif /* PULSE_H */