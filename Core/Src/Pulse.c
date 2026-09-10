#include "Pulse.h"
#include "hrtim.h"
#include "tim.h"
#include <math.h>

/* 全局控制器实体 */
Pulse_Controller_t g_pulse_ctrl = {
    .channel              = CH1,
    .mode                 = PULSE_MODE_NPULSE,
    .timer_idx            = HRTIM_TIMERINDEX_TIMER_B,
    .timer_id             = HRTIM_TIMERID_TIMER_B,
    .output_ch            = HRTIM_OUTPUT_TB2,
    .output_ch2           = HRTIM_OUTPUT_TB1,
    .pair_idx             = COMP_PAIR_CH1_CH2,
    .polarity             = PULSE_POLARITY_HIGH,
    .is_enabled           = false
};

/* 兼容性全局变量导出 (对齐 main.c 与 WouoUI_user.c) */
volatile uint32_t HRTIM_TIMERINDEX_TIMER_X = HRTIM_TIMERINDEX_TIMER_B;
volatile uint32_t HRTIM_TIMERID_TIMER_X    = HRTIM_TIMERID_TIMER_B;
volatile uint32_t HRTIM_OUTPUT_TXX         = HRTIM_OUTPUT_TB2;
volatile uint8_t  PULSE_MODE               = PULSE_MODE_NPULSE;
volatile bool     PULSE_OUT_ENABLED        = false;
volatile bool     PULSE_POLARITY           = PULSE_POLARITY_HIGH;
volatile uint32_t lpwm_arr                 = 0;
volatile uint32_t lpwm_ccr                 = 0;
volatile bool     g_fault_flag             = false;   /* Fault 发生标志 (ISR 置位, main 循环消费) */

/* 长脉冲 (TIM5 软件模式) 剩余脉冲计数, 由 TIM5 中断维护 */
static volatile uint32_t s_pulse_remain    = 0;

/* 短脉冲 (HRTIM) 当前突发剩余脉冲计数, 由 CMP4 周期结束中断维护 */
static volatile uint32_t s_npulse_remain   = 0;

/* 短脉冲 (HRTIM) 本次猝发的脉冲总数 (PRF 周期猝发复读) */
static volatile uint32_t s_npulse_count    = 1;

/* Burst PRF 猝发重复频率 (Hz), 0 = 单次触发 */
static volatile uint32_t s_burst_prf_hz    = 0;

/* HRTIM 硬件配置结构体 */
HRTIM_TimeBaseCfgTypeDef TimeBaseCfg = {0};
HRTIM_TimerCtlTypeDef    TimerCtl    = {0};
HRTIM_TimerCfgTypeDef    TimerCfg    = {0};
HRTIM_CompareCfgTypeDef  CompareCfg  = {0};
HRTIM_OutputCfgTypeDef   OutputCfg   = {0};

/* 私有函数：根据目标微秒时间计算最优 HRTIM 分频比与计数值 */
static bool Pulse_CalcPrescalerAndCounts(float time_us, uint32_t *out_prescaler, float *out_freq, uint32_t *out_counts)
{
    if (time_us <= 0.0f) return false;

    uint32_t prescaler_value;
    float current_hrtim_freq;

    if (time_us <= 11.8f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_MUL32;
        current_hrtim_freq = 170000000.0f * 32.0f;
    }
    else if (time_us <= 23.8f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_MUL16;
        current_hrtim_freq = 170000000.0f * 16.0f;
    }
    else if (time_us <= 47.9f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_MUL8;
        current_hrtim_freq = 170000000.0f * 8.0f;
    }
    else if (time_us <= 96.0f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_MUL4;
        current_hrtim_freq = 170000000.0f * 4.0f;
    }
    else if (time_us <= 192.4f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_MUL2;
        current_hrtim_freq = 170000000.0f * 2.0f;
    }
    else if (time_us <= 385.1f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_DIV1;
        current_hrtim_freq = 170000000.0f * 1.0f;
    }
    else if (time_us <= 770.4f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_DIV2;
        current_hrtim_freq = 170000000.0f / 2.0f;
    }
    else
    {
        prescaler_value = HRTIM_PRESCALERRATIO_DIV4;
        current_hrtim_freq = 170000000.0f / 4.0f;
    }

    if (out_prescaler) *out_prescaler = prescaler_value;
    if (out_freq) *out_freq = current_hrtim_freq;

    if (out_counts)
    {
        float time_s = US_TO_S(time_us);
        uint32_t compare_value = (uint32_t)roundf(time_s * current_hrtim_freq);
        if (compare_value > 0xFFDF) compare_value = 0xFFDF;
        else if (compare_value < 96) compare_value = 96;
        *out_counts = compare_value;
    }

    return true;
}

/* 同步全局兼容变量到 g_pulse_ctrl */
static void Pulse_SyncContext(void)
{
    HRTIM_TIMERINDEX_TIMER_X = g_pulse_ctrl.timer_idx;
    HRTIM_TIMERID_TIMER_X    = g_pulse_ctrl.timer_id;
    HRTIM_OUTPUT_TXX         = g_pulse_ctrl.output_ch;
    /* 彻底删除此行：PULSE_MODE = g_pulse_ctrl.mode; */
    PULSE_OUT_ENABLED        = g_pulse_ctrl.is_enabled;
    PULSE_POLARITY           = g_pulse_ctrl.polarity;
}

/* 互补通道对 GPIO 映射 (严格遵循 HARDWARE.md §5.1:
   同一定时器 Tx1(主路/参考) 与 Tx2(互补) 由 HRTIM 死区硬件生成) */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin_main;   /* Tx1 主路引脚 */
    uint16_t      pin_comp;   /* Tx2 互补引脚 */
} Pulse_CompPinMap_t;

static const Pulse_CompPinMap_t s_comp_pin_map[3] = {
    /* pair0: CH1&CH2 -> Timer B: TB1(PA10,CH2)=主路, TB2(PA11,CH1)=互补 */
    { GPIOA, HRT_CHB1_Pin, HRT_CHB2_Pin },
    /* pair1: CH3&CH4 -> Timer A: TA1(PA8,CH4)=主路, TA2(PA9,CH3)=互补 */
    { GPIOA, HRT_CHA1_Pin, HRT_CHA2_Pin },
    /* pair2: CH5&CH6 -> Timer D: TD1(PB14,CH6)=主路, TD2(PB15,CH5)=互补 */
    { GPIOB, HRT_CHD1_Pin, HRT_CHD2_Pin },
};

static const Pulse_CompPinMap_t *Pulse_CompPinGet(void)
{
    uint8_t p = g_pulse_ctrl.pair_idx;
    if (p > 2U) p = 0U;
    return &s_comp_pin_map[p];
}

/* 互补引脚电平控制 (TIM5 超长互补模式软件死区使用, BSRR 原子写杜绝共态导通) */
static void Pulse_CompPinMainSetActive(void)
{
    const Pulse_CompPinMap_t *m = Pulse_CompPinGet();
    m->port->BSRR = m->pin_main;
}
static void Pulse_CompPinMainSetInactive(void)
{
    const Pulse_CompPinMap_t *m = Pulse_CompPinGet();
    m->port->BSRR = (uint32_t)m->pin_main << 16U;
}
static void Pulse_CompPinCompSetActive(void)
{
    const Pulse_CompPinMap_t *m = Pulse_CompPinGet();
    m->port->BSRR = m->pin_comp;
}
static void Pulse_CompPinCompSetInactive(void)
{
    const Pulse_CompPinMap_t *m = Pulse_CompPinGet();
    m->port->BSRR = (uint32_t)m->pin_comp << 16U;
}

/* 通道选择：严格按照 HARDWARE.md §5.1 表格映射 CH1 ~ CH6 (Timer C 已剥离发波) */
void Pulse_Select_Output(uint8_t CHx)
{
    bool was_enabled = g_pulse_ctrl.is_enabled;
    if (was_enabled) Pulse_Disable_Output(); // 先关断旧通道，严禁打出寄生脉冲

    g_pulse_ctrl.mode = PULSE_MODE; // 同步当前 UI 所选模式

    uint32_t _pm = __get_PRIMASK(); __disable_irq();

    g_pulse_ctrl.channel = CHx;

    switch (CHx)
    {
        case CH1: /* HRTIM1_CHB2 (PA11 -> Y1) */
            g_pulse_ctrl.timer_idx = HRTIM_TIMERINDEX_TIMER_B;
            g_pulse_ctrl.timer_id  = HRTIM_TIMERID_TIMER_B;
            g_pulse_ctrl.output_ch = HRTIM_OUTPUT_TB2;
            break;
        case CH2: /* HRTIM1_CHB1 (PA10 -> Y2) */
            g_pulse_ctrl.timer_idx = HRTIM_TIMERINDEX_TIMER_B;
            g_pulse_ctrl.timer_id  = HRTIM_TIMERID_TIMER_B;
            g_pulse_ctrl.output_ch = HRTIM_OUTPUT_TB1;
            break;
        case CH3: /* HRTIM1_CHA2 (PA9 -> Y3) */
            g_pulse_ctrl.timer_idx = HRTIM_TIMERINDEX_TIMER_A;
            g_pulse_ctrl.timer_id  = HRTIM_TIMERID_TIMER_A;
            g_pulse_ctrl.output_ch = HRTIM_OUTPUT_TA2;
            break;
        case CH4: /* HRTIM1_CHA1 (PA8 -> Y4) */
            g_pulse_ctrl.timer_idx = HRTIM_TIMERINDEX_TIMER_A;
            g_pulse_ctrl.timer_id  = HRTIM_TIMERID_TIMER_A;
            g_pulse_ctrl.output_ch = HRTIM_OUTPUT_TA1;
            break;
        case CH5: /* HRTIM1_CHD2 (PB15 -> Y5) */
            g_pulse_ctrl.timer_idx = HRTIM_TIMERINDEX_TIMER_D;
            g_pulse_ctrl.timer_id  = HRTIM_TIMERID_TIMER_D;
            g_pulse_ctrl.output_ch = HRTIM_OUTPUT_TD2;
            break;
        case CH6: /* HRTIM1_CHD1 (PB14 -> Y6) */
            g_pulse_ctrl.timer_idx = HRTIM_TIMERINDEX_TIMER_D;
            g_pulse_ctrl.timer_id  = HRTIM_TIMERID_TIMER_D;
            g_pulse_ctrl.output_ch = HRTIM_OUTPUT_TD1;
            break;
        default:
            g_pulse_ctrl.timer_idx = HRTIM_TIMERINDEX_TIMER_B;
            g_pulse_ctrl.timer_id  = HRTIM_TIMERID_TIMER_B;
            g_pulse_ctrl.output_ch = HRTIM_OUTPUT_TB2;
            break;
    }

    Pulse_SyncContext();

    __set_PRIMASK(_pm);

    if (PULSE_MODE == PULSE_MODE_NPULSE)
        Pulse_nPulse_Init();
    else if (PULSE_MODE == PULSE_MODE_NPULSE_LONG)
        Pulse_nPulseLong_Init();
    else if (PULSE_MODE == PULSE_MODE_DPULSE)
        Pulse_dPulse_Init();
    else if (PULSE_MODE == PULSE_MODE_PWM)
        Pulse_PWM_Init();
    else if (PULSE_MODE == PULSE_MODE_PWM_LONG)
        Pulse_lPWM_Init();

    Pulse_SetPulsePolarity_High();

    if (was_enabled) Pulse_Enable_Output();
}

void Pulse_Enable_Output(void)
{
    g_pulse_ctrl.is_enabled = true;
    Pulse_SyncContext();

    if (PULSE_MODE == PULSE_MODE_NPULSE)
    {
        /* 使能 CMP4 周期结束中断 (短脉冲多脉冲软件重触发) */
        __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_TIM_IT_CMP4);
    }

    HAL_HRTIM_WaveformOutputStart(&hhrtim1, g_pulse_ctrl.output_ch);
    HAL_HRTIM_WaveformCountStart(&hhrtim1, g_pulse_ctrl.timer_id);

    if (PULSE_MODE == PULSE_MODE_COMP_PWM)
    {
        /* 互补模式: 同时使能 Tx1(主路) 与 Tx2(互补) 两路输出 */
        HAL_HRTIM_WaveformOutputStart(&hhrtim1, g_pulse_ctrl.output_ch2);
    }

    if (PULSE_MODE == PULSE_MODE_PWM_LONG)
    {
        __HAL_TIM_SET_COUNTER(&htim5, 0);
        if (lpwm_ccr > 0) { // 首周期立即输出有效电平，无需等待漫长的第一个溢出周期
            if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
                Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();
            else
                Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
        }
        HAL_TIM_Base_Start_IT(&htim5);
        HAL_TIM_OC_Start_IT(&htim5, TIM_CHANNEL_1);
    }
    else if (PULSE_MODE == PULSE_MODE_COMP_PWM_LONG)
    {
        /* 超长互补: 两路均先拉低, 启动 TIM5 后由 CC3 死区点再开主路, 严禁直通 */
        __HAL_TIM_SET_COUNTER(&htim5, 0);
        Pulse_CompPinMainSetInactive();
        Pulse_CompPinCompSetInactive();
        HAL_TIM_Base_Start_IT(&htim5);
        HAL_TIM_OC_Start_IT(&htim5, TIM_CHANNEL_1);
        HAL_TIM_OC_Start_IT(&htim5, TIM_CHANNEL_2);
        HAL_TIM_OC_Start_IT(&htim5, TIM_CHANNEL_3);
    }

    /* PRF 猝发重复: 频率 > 0 时立即发首帧并启动周期重发 (仅 N 脉冲模式) */
    if (PULSE_MODE == PULSE_MODE_NPULSE)
    {
        Pulse_BurstPRF_Start();
    }
}

void Pulse_Disable_Output(void)
{
    g_pulse_ctrl.is_enabled = false;
    Pulse_SyncContext();
    s_npulse_remain = 0;   /* 关断输出时中止短脉冲重触发链 */
    Pulse_BurstPRF_Stop();      /* 停止 PRF 周期猝发 */
    Pulse_Frame_SetInactive();  /* 帧标记拉低 */

    /* 关闭 CMP4 周期结束中断 (仅短脉冲多脉冲使用, 其它模式无需) */
    __HAL_HRTIM_TIMER_DISABLE_IT(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_TIM_IT_CMP4);

    HAL_HRTIM_WaveformOutputStop(&hhrtim1, g_pulse_ctrl.output_ch);
    HAL_HRTIM_WaveformCountStop(&hhrtim1, g_pulse_ctrl.timer_id);

    if (PULSE_MODE == PULSE_MODE_COMP_PWM)
    {
        /* 互补模式: 同时关断 Tx1 与 Tx2 两路输出 */
        HAL_HRTIM_WaveformOutputStop(&hhrtim1, g_pulse_ctrl.output_ch2);
    }

    if (PULSE_MODE == PULSE_MODE_NPULSE_LONG)
    {
        s_pulse_remain = 0;
        __HAL_TIM_DISABLE(&htim5);
        __HAL_TIM_DISABLE_IT(&htim5, TIM_IT_UPDATE | TIM_IT_CC1);
        Pulse_LongPin_SetInactive();
    }
    else if (PULSE_MODE == PULSE_MODE_PWM_LONG)
    {
        HAL_TIM_Base_Stop_IT(&htim5);
        HAL_TIM_OC_Stop_IT(&htim5, TIM_CHANNEL_1);
        Pulse_LongPin_SetInactive();
    }
    else if (PULSE_MODE == PULSE_MODE_COMP_PWM_LONG)
    {
        HAL_TIM_Base_Stop_IT(&htim5);
        HAL_TIM_OC_Stop_IT(&htim5, TIM_CHANNEL_1);
        HAL_TIM_OC_Stop_IT(&htim5, TIM_CHANNEL_2);
        HAL_TIM_OC_Stop_IT(&htim5, TIM_CHANNEL_3);
        Pulse_CompPinMainSetInactive();
        Pulse_CompPinCompSetInactive();
    }
}

void Pulse_SetPulsePolarity_High(void)
{
    const bool was_enabled = g_pulse_ctrl.is_enabled;
    g_pulse_ctrl.polarity = PULSE_POLARITY_HIGH;
    Pulse_SyncContext();

    HRTIM_OutputCfgTypeDef OutCfg = {0};

    Pulse_Disable_Output();
    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

    if (PULSE_MODE == PULSE_MODE_NPULSE || PULSE_MODE == PULSE_MODE_PWM)
    {
        OutCfg.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
        OutCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1;
        OutCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2;
        OutCfg.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
        OutCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
        OutCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
        OutCfg.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
        OutCfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
        if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, g_pulse_ctrl.timer_idx, g_pulse_ctrl.output_ch, &OutCfg) != HAL_OK)
        {
            Error_Handler();
        }
    }
    else if (PULSE_MODE == PULSE_MODE_DPULSE)
    {
        OutCfg.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
        OutCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1 | HRTIM_OUTPUTSET_TIMCMP3;
        OutCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2 | HRTIM_OUTPUTRESET_TIMCMP4;
        OutCfg.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
        OutCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
        OutCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
        OutCfg.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
        OutCfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
        if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, g_pulse_ctrl.timer_idx, g_pulse_ctrl.output_ch, &OutCfg) != HAL_OK)
        {
            Error_Handler();
        }
    }

    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);
    if (was_enabled) Pulse_Enable_Output();
}

void Pulse_SetPulsePolarity_Low(void)
{
    const bool was_enabled = g_pulse_ctrl.is_enabled;
    g_pulse_ctrl.polarity = PULSE_POLARITY_LOW;
    Pulse_SyncContext();

    HRTIM_OutputCfgTypeDef OutCfg = {0};

    Pulse_Disable_Output();
    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

    if (PULSE_MODE == PULSE_MODE_NPULSE || PULSE_MODE == PULSE_MODE_PWM)
    {
        OutCfg.Polarity = HRTIM_OUTPUTPOLARITY_LOW;
        OutCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1;
        OutCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2;
        OutCfg.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
        OutCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
        OutCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
        OutCfg.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
        OutCfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
        if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, g_pulse_ctrl.timer_idx, g_pulse_ctrl.output_ch, &OutCfg) != HAL_OK)
        {
            Error_Handler();
        }
    }
    else if (PULSE_MODE == PULSE_MODE_DPULSE)
    {
        OutCfg.Polarity = HRTIM_OUTPUTPOLARITY_LOW;
        OutCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1 | HRTIM_OUTPUTSET_TIMCMP3;
        OutCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2 | HRTIM_OUTPUTRESET_TIMCMP4;
        OutCfg.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
        OutCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
        OutCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
        OutCfg.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
        OutCfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
        if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, g_pulse_ctrl.timer_idx, g_pulse_ctrl.output_ch, &OutCfg) != HAL_OK)
        {
            Error_Handler();
        }
    }

    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);
    if (was_enabled) Pulse_Enable_Output();
}

/* N 脉冲 (短脉冲) 相关函数 */

/* 软件重触发方案的固定开销补偿: 每个脉冲间隙会多出中断响应 + TxRST 重触发的时间,
   实测稳定约 0.9us, (补偿后最小可实现实际间隔 ≈ 0.9us, 即该开销本身) */
#define NPULSE_INTERVAL_COMP_US   0.9f
#define NPULSE_INTERVAL_MIN_US    0.05f   /* 补偿后最小硬件间隔, 避免 tick 取整退化为 0 */

/* 为 N 脉冲选择分频比并计算周期与比较值:
   单周期 = PW + Interval, 需保证整周期能容纳在 16bit PER (0xFFDF) 内 */
static bool Pulse_CalcNpulseTiming(float pw_us, float interval_us,
                                   uint32_t *out_psc, uint32_t *out_period, uint32_t *out_cmp2)
{
    const float total_us = pw_us + interval_us;

    /* 候选分频档: 由快到慢 (快 = 更高分辨率) */
    static const struct { uint32_t psc; float freq; } tab[8] =
    {
        { HRTIM_PRESCALERRATIO_MUL32, 170000000.0f * 32.0f },
        { HRTIM_PRESCALERRATIO_MUL16, 170000000.0f * 16.0f },
        { HRTIM_PRESCALERRATIO_MUL8,  170000000.0f * 8.0f  },
        { HRTIM_PRESCALERRATIO_MUL4,  170000000.0f * 4.0f  },
        { HRTIM_PRESCALERRATIO_MUL2,  170000000.0f * 2.0f  },
        { HRTIM_PRESCALERRATIO_DIV1,  170000000.0f * 1.0f  },
        { HRTIM_PRESCALERRATIO_DIV2,  170000000.0f / 2.0f  },
        { HRTIM_PRESCALERRATIO_DIV4,  170000000.0f / 4.0f  },
    };

    /* 优先选最快的、且整周期与 PW 均能被正确表达的分频档 */
    for (uint8_t i = 0; i < 8; i++)
    {
        uint32_t period = (uint32_t)roundf(US_TO_S(total_us) * tab[i].freq);
        uint32_t cmp2   = (uint32_t)roundf(US_TO_S(pw_us) * tab[i].freq);
        if (period <= 0xFFDFU && cmp2 >= 96U && cmp2 < period)
        {
            *out_psc    = tab[i].psc;
            *out_period = period;
            *out_cmp2   = cmp2;
            return true;
        }
    }

    /* 兜底: 最慢分频档, 周期截断到 16bit 上限 (超出部分说明总周期已超 HRTIM 硬件极限) */
    {
        const float freq = 170000000.0f / 4.0f;
        uint32_t cmp2 = (uint32_t)roundf(US_TO_S(pw_us) * freq);
        if (cmp2 > 0xFFDFU) cmp2 = 0xFFDFU;
        else if (cmp2 < 96U) cmp2 = 96U;

        *out_psc    = HRTIM_PRESCALERRATIO_DIV4;
        *out_period = 0xFFDFU;
        if (cmp2 >= *out_period) cmp2 = *out_period - 1U;
        *out_cmp2   = cmp2;
        return true;
    }
}

bool Pulse_nPulse_SetPW(float pw, float interval_us, uint32_t count)
{
    if (pw < 0.01f || pw > 1500.0f)      return false;
    if (count < 1 || count > 100)        return false;
    if (count > 1 && (interval_us < 1.0f || interval_us > 1500.0f)) return false;

    s_npulse_count = count;   /* 记录猝发脉冲总数, 供 PRF 周期猝发复读 */

    const bool was_enabled = g_pulse_ctrl.is_enabled; // 暂存使能状态
    uint32_t prescaler_value;
    uint32_t period_value;
    uint32_t compare_value;
    HRTIM_TimeBaseCfgTypeDef ScalerCfg = {0};
    HRTIM_CompareCfgTypeDef CmpCfg = {0};

    if (count == 1)
    {
        /* 脉冲数 = 1: 完全沿用既有单脉冲行为 (Period=65503, CMP2=PW, REP=0), 保证波形一致 */
        if (!Pulse_CalcPrescalerAndCounts(pw, &prescaler_value, NULL, &compare_value))
        {
            return false;
        }
        period_value = 65503U;
    }
    else
    {
        /* N 脉冲: 周期 = PW + (Interval - 补偿), CMP2 = PW, CMP4 = 周期末检测点, 由软件重触发连发 N 个脉冲 */
        float interval_comp = interval_us - NPULSE_INTERVAL_COMP_US;
        if (interval_comp < NPULSE_INTERVAL_MIN_US) interval_comp = NPULSE_INTERVAL_MIN_US;

        if (!Pulse_CalcNpulseTiming(pw, interval_comp, &prescaler_value, &period_value, &compare_value))
        {
            return false;
        }
    }

    hhrtim1.Instance->sMasterRegs.MCR &= ~g_pulse_ctrl.timer_id;

    // Pulse_Disable_Output();
    // HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

    ScalerCfg.Period = period_value;
    ScalerCfg.RepetitionCounter = 0;   /* 短脉冲不依赖重复计数器连发 (REPxR 在单次模式下不产生多周期) */
    ScalerCfg.PrescalerRatio = prescaler_value;
    /* 多脉冲用「可重触发单次模式」, 由 CMP4 周期结束中断软件重发下一脉冲; 单脉冲保持原非重触发模式 */
    ScalerCfg.Mode = (count == 1) ? HRTIM_MODE_SINGLESHOT : HRTIM_MODE_SINGLESHOT_RETRIGGERABLE;
    if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, g_pulse_ctrl.timer_idx, &ScalerCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CmpCfg.CompareValue = compare_value;
    CmpCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
    CmpCfg.AutoDelayedTimeout = 0x0000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_2, &CmpCfg) != HAL_OK)
    {
        Error_Handler();
    }

    /* CMP4 = 周期末 - 1: 作为「单周期结束」检测点, 触发软件重发下一脉冲 (对 count==1 无副作用) */
    CmpCfg.CompareValue = period_value - 1U;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_4, &CmpCfg) != HAL_OK)
    {
        Error_Handler();
    }

    hhrtim1.Instance->sTimerxRegs[g_pulse_ctrl.timer_idx].CNTxR = 0;
    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

    if (was_enabled) {
        hhrtim1.Instance->sMasterRegs.MCR |= g_pulse_ctrl.timer_id;
    }

    return true;
}

void Pulse_nPulse_Init(void)
{
    g_pulse_ctrl.mode = PULSE_MODE_NPULSE;
    Pulse_SyncContext();
    s_npulse_remain = 0;

    TimeBaseCfg.Period = 65503;
    TimeBaseCfg.RepetitionCounter = 0;
    TimeBaseCfg.PrescalerRatio = HRTIM_PRESCALERRATIO_MUL32;
    TimeBaseCfg.Mode = HRTIM_MODE_SINGLESHOT;
    if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, g_pulse_ctrl.timer_idx, &TimeBaseCfg) != HAL_OK)
    {
        Error_Handler();
    }

    TimerCtl.UpDownMode = HRTIM_TIMERUPDOWNMODE_UP;
    TimerCtl.TrigHalf = HRTIM_TIMERTRIGHALF_DISABLED;
    TimerCtl.GreaterCMP1 = HRTIM_TIMERGTCMP1_EQUAL;
    TimerCtl.DualChannelDacEnable = HRTIM_TIMER_DCDE_DISABLED;
    if (HAL_HRTIM_WaveformTimerControl(&hhrtim1, g_pulse_ctrl.timer_idx, &TimerCtl) != HAL_OK)
    {
        Error_Handler();
    }

    TimerCfg.InterruptRequests = HRTIM_TIM_IT_CMP4;   /* CMP4 周期结束中断: 多脉冲软件重触发检测点 */
    TimerCfg.DMARequests = HRTIM_TIM_DMA_NONE;
    TimerCfg.DMASrcAddress = 0x0000;
    TimerCfg.DMADstAddress = 0x0000;
    TimerCfg.DMASize = 0x1;
    TimerCfg.HalfModeEnable = HRTIM_HALFMODE_DISABLED;
    TimerCfg.InterleavedMode = HRTIM_INTERLEAVED_MODE_DISABLED;
    TimerCfg.StartOnSync = HRTIM_SYNCSTART_DISABLED;
    TimerCfg.ResetOnSync = HRTIM_SYNCRESET_DISABLED;
    TimerCfg.DACSynchro = HRTIM_DACSYNC_NONE;
    TimerCfg.PreloadEnable = HRTIM_PRELOAD_DISABLED; // 开启预装载防抖
    TimerCfg.UpdateGating = HRTIM_UPDATEGATING_INDEPENDENT;
    TimerCfg.BurstMode = HRTIM_TIMERBURSTMODE_MAINTAINCLOCK;
    TimerCfg.RepetitionUpdate = HRTIM_UPDATEONREPETITION_DISABLED;
    TimerCfg.PushPull = HRTIM_TIMPUSHPULLMODE_DISABLED;
    TimerCfg.FaultEnable = HRTIM_TIMFAULTENABLE_FAULT2;
    TimerCfg.FaultLock = HRTIM_TIMFAULTLOCK_READWRITE;
    TimerCfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_DISABLED;
    TimerCfg.DelayedProtectionMode = HRTIM_TIMER_A_B_C_DELAYEDPROTECTION_DISABLED;
    TimerCfg.UpdateTrigger = HRTIM_TIMUPDATETRIGGER_NONE;
    TimerCfg.ResetTrigger = HRTIM_TIMRESETTRIGGER_NONE;
    TimerCfg.ResetUpdate = HRTIM_TIMUPDATEONRESET_DISABLED;
    TimerCfg.ReSyncUpdate = HRTIM_TIMERESYNC_UPDATE_UNCONDITIONAL;
    if (HAL_HRTIM_WaveformTimerConfig(&hhrtim1, g_pulse_ctrl.timer_idx, &TimerCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CompareCfg.CompareValue = 0;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_1, &CompareCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CompareCfg.CompareValue = 5435;
    CompareCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
    CompareCfg.AutoDelayedTimeout = 0x0000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_2, &CompareCfg) != HAL_OK)
    {
        Error_Handler();
    }

    OutputCfg.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
    OutputCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1;
    OutputCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2;
    OutputCfg.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
    OutputCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    OutputCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    OutputCfg.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    OutputCfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, g_pulse_ctrl.timer_idx, g_pulse_ctrl.output_ch, &OutputCfg) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_HRTIM_MspPostInit(&hhrtim1);

    Pulse_nPulse_SetPW(1.0f, 1.0f, 1);
    Pulse_SetPulsePolarity_High();
}

/* 软件复位 HRTIM 当前定时器 (等价于外部触发 TxRST), 用于多脉冲重触发 */
static void Pulse_Hrtim_SoftwareReset(void)
{
    switch (g_pulse_ctrl.timer_idx)
    {
        case HRTIM_TIMERINDEX_TIMER_A: HRTIM1->sCommonRegs.CR2 = HRTIM_CR2_TARST; break;
        case HRTIM_TIMERINDEX_TIMER_B: HRTIM1->sCommonRegs.CR2 = HRTIM_CR2_TBRST; break;
        case HRTIM_TIMERINDEX_TIMER_C: HRTIM1->sCommonRegs.CR2 = HRTIM_CR2_TCRST; break;
        case HRTIM_TIMERINDEX_TIMER_D: HRTIM1->sCommonRegs.CR2 = HRTIM_CR2_TDRST; break;
        default: break;
    }
}

/* 外部触发入口: 记录本次突发需要发出的脉冲个数 (首个脉冲由调用方 TxRST 触发) */
void Pulse_nPulse_OnTrigger(uint32_t count)
{
    if (count < 1) count = 1;
    if (count > 100) count = 100;
    s_npulse_count  = count;
    s_npulse_remain = count;
}

/* CMP4 周期结束中断: 单周期发完后, 若还有剩余脉冲则重发, 否则停止 */
void Pulse_nPulse_OnPeriodEnd(void)
{
    if (s_npulse_remain > 1)
    {
        s_npulse_remain--;
        Pulse_Hrtim_SoftwareReset();   /* 重触发下一个脉冲 */
    }
    else
    {
        s_npulse_remain = 0;           /* 脉冲串结束 (单次模式下定时器已自动停止) */
        Pulse_Frame_SetInactive();     /* 帧标记拉低: 一帧猝发结束 */
    }
}

/* 双脉冲相关函数 */
bool Pulse_dPulse_SetPW(int32_t pw1, int32_t interval, int32_t pw2)
{
    if (pw1 < 1 || pw1 > 200 || pw2 < 1 || pw2 > 200 || interval < 1 || interval > 200)
    {
        return false;
    }

    const bool was_enabled = g_pulse_ctrl.is_enabled; // 暂存使能状态
    float ptotal = (float)(pw1 + interval + pw2);
    uint32_t prescaler_value;
    float current_hrtim_freq;
    uint32_t compare_value2, compare_value3, compare_value4;
    HRTIM_TimeBaseCfgTypeDef ScalerCfg = {0};
    HRTIM_CompareCfgTypeDef CmpCfg = {0};

    Pulse_CalcPrescalerAndCounts(ptotal, &prescaler_value, &current_hrtim_freq, NULL);

    float pw1_s      = US_TO_S(pw1);
    float interval_s = US_TO_S(interval);
    float pw2_s      = US_TO_S(pw2);

    compare_value2 = (uint32_t)roundf(pw1_s * current_hrtim_freq);
    compare_value3 = compare_value2 + (uint32_t)roundf(interval_s * current_hrtim_freq);
    compare_value4 = compare_value3 + (uint32_t)roundf(pw2_s * current_hrtim_freq);

    if (compare_value2 > 0xFFDF) compare_value2 = 0xFFDF;
    else if (compare_value2 < 96) compare_value2 = 96;

    if (compare_value3 > 0xFFDF) compare_value3 = 0xFFDF;
    else if (compare_value3 < 96) compare_value3 = 96;

    if (compare_value4 > 0xFFDF) compare_value4 = 0xFFDF;
    else if (compare_value4 < 96) compare_value4 = 96;

    hhrtim1.Instance->sMasterRegs.MCR &= ~g_pulse_ctrl.timer_id;

    // Pulse_Disable_Output();
    // HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

    ScalerCfg.Period = 65503;
    ScalerCfg.RepetitionCounter = 0;
    ScalerCfg.PrescalerRatio = prescaler_value;
    ScalerCfg.Mode = HRTIM_MODE_SINGLESHOT;
    if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, g_pulse_ctrl.timer_idx, &ScalerCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CmpCfg.CompareValue = 0;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_1, &CmpCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CmpCfg.CompareValue = compare_value2;
    CmpCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
    CmpCfg.AutoDelayedTimeout = 0x0000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_2, &CmpCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CmpCfg.CompareValue = compare_value3;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_3, &CmpCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CmpCfg.CompareValue = compare_value4;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_4, &CmpCfg) != HAL_OK)
    {
        Error_Handler();
    }

    hhrtim1.Instance->sTimerxRegs[g_pulse_ctrl.timer_idx].CNTxR = 0;
    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

    if (was_enabled) {
        hhrtim1.Instance->sMasterRegs.MCR |= g_pulse_ctrl.timer_id;
    }

    return true;
}

void Pulse_dPulse_Init(void)
{
    g_pulse_ctrl.mode = PULSE_MODE_DPULSE;
    Pulse_SyncContext();

    TimeBaseCfg.Period = 65503;
    TimeBaseCfg.RepetitionCounter = 0;
    TimeBaseCfg.PrescalerRatio = HRTIM_PRESCALERRATIO_MUL32;
    TimeBaseCfg.Mode = HRTIM_MODE_SINGLESHOT;
    if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, g_pulse_ctrl.timer_idx, &TimeBaseCfg) != HAL_OK)
    {
        Error_Handler();
    }

    TimerCtl.UpDownMode = HRTIM_TIMERUPDOWNMODE_UP;
    TimerCtl.TrigHalf = HRTIM_TIMERTRIGHALF_DISABLED;
    TimerCtl.GreaterCMP3 = HRTIM_TIMERGTCMP3_EQUAL;
    TimerCtl.GreaterCMP1 = HRTIM_TIMERGTCMP1_EQUAL;
    TimerCtl.DualChannelDacEnable = HRTIM_TIMER_DCDE_DISABLED;
    if (HAL_HRTIM_WaveformTimerControl(&hhrtim1, g_pulse_ctrl.timer_idx, &TimerCtl) != HAL_OK)
    {
        Error_Handler();
    }

    TimerCfg.InterruptRequests = HRTIM_TIM_IT_NONE;
    TimerCfg.DMARequests = HRTIM_TIM_DMA_NONE;
    TimerCfg.DMASrcAddress = 0x0000;
    TimerCfg.DMADstAddress = 0x0000;
    TimerCfg.DMASize = 0x1;
    TimerCfg.HalfModeEnable = HRTIM_HALFMODE_DISABLED;
    TimerCfg.InterleavedMode = HRTIM_INTERLEAVED_MODE_DISABLED;
    TimerCfg.StartOnSync = HRTIM_SYNCSTART_DISABLED;
    TimerCfg.ResetOnSync = HRTIM_SYNCRESET_DISABLED;
    TimerCfg.DACSynchro = HRTIM_DACSYNC_NONE;
    TimerCfg.PreloadEnable = HRTIM_PRELOAD_DISABLED; // 开启预装载防抖
    TimerCfg.UpdateGating = HRTIM_UPDATEGATING_INDEPENDENT;
    TimerCfg.BurstMode = HRTIM_TIMERBURSTMODE_MAINTAINCLOCK;
    TimerCfg.RepetitionUpdate = HRTIM_UPDATEONREPETITION_DISABLED;
    TimerCfg.PushPull = HRTIM_TIMPUSHPULLMODE_DISABLED;
    TimerCfg.FaultEnable = HRTIM_TIMFAULTENABLE_FAULT2;
    TimerCfg.FaultLock = HRTIM_TIMFAULTLOCK_READWRITE;
    TimerCfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_DISABLED;
    TimerCfg.DelayedProtectionMode = HRTIM_TIMER_A_B_C_DELAYEDPROTECTION_DISABLED;
    TimerCfg.UpdateTrigger = HRTIM_TIMUPDATETRIGGER_NONE;
    TimerCfg.ResetTrigger = HRTIM_TIMRESETTRIGGER_NONE;
    TimerCfg.ResetUpdate = HRTIM_TIMUPDATEONRESET_DISABLED;
    TimerCfg.ReSyncUpdate = HRTIM_TIMERESYNC_UPDATE_UNCONDITIONAL;
    if (HAL_HRTIM_WaveformTimerConfig(&hhrtim1, g_pulse_ctrl.timer_idx, &TimerCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CompareCfg.CompareValue = 0;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_1, &CompareCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CompareCfg.CompareValue = 5435;
    CompareCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
    CompareCfg.AutoDelayedTimeout = 0x0000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_2, &CompareCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CompareCfg.CompareValue = 6000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_3, &CompareCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CompareCfg.CompareValue = 510;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_4, &CompareCfg) != HAL_OK)
    {
        Error_Handler();
    }

    OutputCfg.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
    OutputCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1 | HRTIM_OUTPUTSET_TIMCMP3;
    OutputCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2 | HRTIM_OUTPUTRESET_TIMCMP4;
    OutputCfg.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
    OutputCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    OutputCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    OutputCfg.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    OutputCfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, g_pulse_ctrl.timer_idx, g_pulse_ctrl.output_ch, &OutputCfg) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_HRTIM_MspPostInit(&hhrtim1);

    Pulse_dPulse_SetPW(5, 5, 5);
    Pulse_SetPulsePolarity_High();
}

/* PWM 相关函数 */
bool Pulse_PWM_SetPW(float period_us, int32_t duty_cycle_percent)
{
    /* ---------- 1. 入参合法性 ---------- */
    if (period_us < 1.0f || period_us > 1500.0f ||
        duty_cycle_percent < 0 || duty_cycle_percent > 100)
    {
        return false;
    }

    /* CMP 最小值随分频档变化 (RM0440: CK_PSC=0->0x60, 1->0x30, 2->0x18, 3->0x0C, 4->0x06, >=5->0x03)
       低于该值的比较事件可能被硬件漏掉 -> Reset 不发生 -> 输出被卡在有效电平(等效 100% 占空) */
    static const uint16_t cmp_min_tab[8] =
        {0x60U, 0x30U, 0x18U, 0x0CU, 0x06U, 0x03U, 0x03U, 0x03U};

    const uint8_t idx         = (uint8_t)g_pulse_ctrl.timer_idx;
    const bool    was_enabled = g_pulse_ctrl.is_enabled;
    bool          must_realign = false;

    uint32_t prescaler_value    = 0U;
    float    current_hrtim_freq = 0.0f;
    uint32_t period_value;
    uint32_t compare_value;
    uint16_t cmp_min;
    HRTIM_TimeBaseCfgTypeDef ScalerCfg = {0};
    HRTIM_CompareCfgTypeDef  CmpCfg    = {0};

    /* ---------- 2. 计算分频比 / PER / CMP2 ---------- */
    if (!Pulse_CalcPrescalerAndCounts(period_us, &prescaler_value, &current_hrtim_freq, NULL))
    {
        return false;
    }

    period_value = (uint32_t)roundf(US_TO_S(period_us) * current_hrtim_freq);
    if (period_value > 0xFFDFU)  period_value = 0xFFDFU;
    else if (period_value < 96U) return false;

    cmp_min = cmp_min_tab[prescaler_value & 0x07U];

    compare_value = (uint32_t)roundf((float)period_value * ((float)duty_cycle_percent / 100.0f));
    if (duty_cycle_percent == 0)
    {
        compare_value = 0U;                        /* 0%: CMP2 与 CMP1 同点, Reset 优先 -> 恒无效电平 */
    }
    else
    {
        if (compare_value < cmp_min)      compare_value = cmp_min;          /* 防漏掉 Reset 事件 */
        if (compare_value >= period_value) compare_value = period_value - 1U; /* 100%: 周期末仍有 Reset */
    }

    /* ---------- 3. 确定性重对齐判据 (用旧 PER, 不用会变化的 CNT) ---------- */
    {
        uint32_t cur_psc = hhrtim1.Instance->sTimerxRegs[idx].TIMxCR & HRTIM_TIMCR_CK_PSC;
        uint32_t old_per = hhrtim1.Instance->sTimerxRegs[idx].PERxR  & 0xFFFFU;

        if (prescaler_value != cur_psc) must_realign = true; /* 换分频档: 时基不连续 */
        if (period_value    <  old_per) must_realign = true; /* PER 收缩: CNT 可能已越过新 PER -> 冲到 0xFFFF */
        if (!was_enabled)               must_realign = true; /* 停机态: CNT 残留旧值, 必须清零 */
        /* PER 增大或不变且 PSC 不变: CNT < old_PER <= new_PER 恒成立, 任意相位 SWU 均安全, 无需停机 */
    }

    if (must_realign && was_enabled)
    {
        Pulse_Disable_Output();      /* 先停计数器与输出, 从根上杜绝畸变周期 */
    }

    /* ---------- 4. 写入新时基与占空比 ---------- */
    ScalerCfg.Period            = period_value;
    ScalerCfg.RepetitionCounter = 0;
    ScalerCfg.PrescalerRatio    = prescaler_value;
    ScalerCfg.Mode              = HRTIM_MODE_CONTINUOUS;
    if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, idx, &ScalerCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CmpCfg.CompareValue       = compare_value;
    CmpCfg.AutoDelayedMode    = HRTIM_AUTODELAYEDMODE_REGULAR;
    CmpCfg.AutoDelayedTimeout = 0x0000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, idx, HRTIM_COMPAREUNIT_2, &CmpCfg) != HAL_OK)
    {
        Error_Handler();
    }

    // /* ---------- 5. 预装载搬移 + 计数器对齐 ---------- */
    // HAL_HRTIM_SoftwareUpdate(&hhrtim1, idx);
    //
    // if (must_realign)
    // {
    //     /* 计数器此刻确定处于停止状态, 可安全写 CNT;
    //        保证重启后首周期完整, 且绝不会从残留大值冲向 0xFFFF */
    //     hhrtim1.Instance->sTimerxRegs[idx].CNTxR = 0U;
    // }

    /* ---------- 5'. 仅在停机态立即生效; 运行态交由 TxRSTU 在周期边界搬移 ---------- */
    if (must_realign)
    {
        HAL_HRTIM_SoftwareUpdate(&hhrtim1, idx);
        hhrtim1.Instance->sTimerxRegs[idx].CNTxR = 0U;
    }



    /* ---------- 6. 恢复输出 ---------- */
    if (must_realign && was_enabled)
    {
        Pulse_Enable_Output();
    }

    return true;
}

void Pulse_PWM_Init(void)
{
    g_pulse_ctrl.mode = PULSE_MODE_PWM;
    Pulse_SyncContext();

    TimeBaseCfg.Period = 65503;
    TimeBaseCfg.RepetitionCounter = 0;
    TimeBaseCfg.PrescalerRatio = HRTIM_PRESCALERRATIO_MUL32;
    TimeBaseCfg.Mode = HRTIM_MODE_CONTINUOUS;
    if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, g_pulse_ctrl.timer_idx, &TimeBaseCfg) != HAL_OK)
    {
        Error_Handler();
    }

    TimerCtl.UpDownMode = HRTIM_TIMERUPDOWNMODE_UP;
    TimerCtl.TrigHalf = HRTIM_TIMERTRIGHALF_DISABLED;
    TimerCtl.GreaterCMP1 = HRTIM_TIMERGTCMP1_EQUAL;
    TimerCtl.DualChannelDacEnable = HRTIM_TIMER_DCDE_DISABLED;
    if (HAL_HRTIM_WaveformTimerControl(&hhrtim1, g_pulse_ctrl.timer_idx, &TimerCtl) != HAL_OK)
    {
        Error_Handler();
    }

    TimerCfg.InterruptRequests = HRTIM_TIM_IT_NONE;
    TimerCfg.DMARequests = HRTIM_TIM_DMA_NONE;
    TimerCfg.DMASrcAddress = 0x0000;
    TimerCfg.DMADstAddress = 0x0000;
    TimerCfg.DMASize = 0x1;
    TimerCfg.HalfModeEnable = HRTIM_HALFMODE_DISABLED;
    TimerCfg.InterleavedMode = HRTIM_INTERLEAVED_MODE_DISABLED;
    TimerCfg.StartOnSync = HRTIM_SYNCSTART_DISABLED;
    TimerCfg.ResetOnSync = HRTIM_SYNCRESET_DISABLED;
    TimerCfg.DACSynchro = HRTIM_DACSYNC_NONE;
    TimerCfg.PreloadEnable = HRTIM_PRELOAD_ENABLED; // 开启预装载防抖
    TimerCfg.UpdateGating = HRTIM_UPDATEGATING_INDEPENDENT;
    TimerCfg.BurstMode = HRTIM_TIMERBURSTMODE_MAINTAINCLOCK;
    TimerCfg.RepetitionUpdate = HRTIM_UPDATEONREPETITION_DISABLED;
    TimerCfg.PushPull = HRTIM_TIMPUSHPULLMODE_DISABLED;
    TimerCfg.FaultEnable = HRTIM_TIMFAULTENABLE_FAULT2;
    TimerCfg.FaultLock = HRTIM_TIMFAULTLOCK_READWRITE;
    TimerCfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_DISABLED;
    TimerCfg.DelayedProtectionMode = HRTIM_TIMER_A_B_C_DELAYEDPROTECTION_DISABLED;
    TimerCfg.UpdateTrigger = HRTIM_TIMUPDATETRIGGER_NONE;
    TimerCfg.ResetTrigger = HRTIM_TIMRESETTRIGGER_NONE;
    TimerCfg.ResetUpdate = HRTIM_TIMUPDATEONRESET_ENABLED;
    TimerCfg.ReSyncUpdate = HRTIM_TIMERESYNC_UPDATE_UNCONDITIONAL;
    if (HAL_HRTIM_WaveformTimerConfig(&hhrtim1, g_pulse_ctrl.timer_idx, &TimerCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CompareCfg.CompareValue = 0;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_1, &CompareCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CompareCfg.CompareValue = 5435;
    CompareCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
    CompareCfg.AutoDelayedTimeout = 0x0000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_2, &CompareCfg) != HAL_OK)
    {
        Error_Handler();
    }

    OutputCfg.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
    OutputCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1;
    OutputCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2;
    OutputCfg.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
    OutputCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    OutputCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    OutputCfg.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    OutputCfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, g_pulse_ctrl.timer_idx, g_pulse_ctrl.output_ch, &OutputCfg) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_HRTIM_MspPostInit(&hhrtim1);

    Pulse_PWM_SetPW(1.0f, 50);
    Pulse_SetPulsePolarity_High();
}

/* 硬件级与软件级紧急关断 (Safe-State 硬件保护) */
void Pulse_EmergencyStop(void)
{
    /* 1. 瞬时强制拉低 PA12 (LOADSW) 切断 12V（零依赖，必须第一句执行） */
    LOADSW_DISABLE();

    /* 2. 检查 HRTIM 时钟是否使能，安全关断全部 8 路高精发波输出 */
    if (RCC->APB2ENR & RCC_APB2ENR_HRTIM1EN) {
        HRTIM1->sCommonRegs.ODISR = 0xFFFFFFFFU;
        HRTIM1->sMasterRegs.MCR &= ~(HRTIM_MCR_MCEN | HRTIM_MCR_TACEN |
                                     HRTIM_MCR_TBCEN | HRTIM_MCR_TCCEN | HRTIM_MCR_TDCEN);
    }

    /* 3. 检查 TIM5 句柄与时钟，防止解引用 NULL 指针引发 HardFault 递归锁死 */
    if ((htim5.Instance != NULL) && (RCC->APB1ENR1 & RCC_APB1ENR1_TIM5EN)) {
        htim5.Instance->CR1 &= ~TIM_CR1_CEN;
        Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
    }

    /* 3b. 互补模式安全态: 主路与互补路两路瞬间同时拉低, 严禁物理电平重叠直通 */
    if (g_pulse_ctrl.mode == PULSE_MODE_COMP_PWM_LONG)
    {
        Pulse_CompPinMainSetInactive();
        Pulse_CompPinCompSetInactive();
    }

    /* 4. 停止 PRF 周期猝发 (TIM3 若已使能) 并拉低帧标记 */
    if (RCC->APB1ENR1 & RCC_APB1ENR1_TIM3EN) {
        TIM3->CR1 &= ~TIM_CR1_CEN;
    }
    Pulse_Frame_SetInactive();

    /* 5. 更新内部状态机为故障保护状态 */
    g_pulse_ctrl.is_enabled = false;
    s_npulse_remain = 0;
    s_pulse_remain = 0;
    Pulse_SyncContext();
}

/* 长时间 N 脉冲相关函数 (TIM5 + GPIO 软件模式) */

/* 拉高长脉冲引脚至有效电平 */
void Pulse_LongPin_SetActive(void)
{
    if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
        Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();
    else
        Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
}

/* 拉低长脉冲引脚至无效电平 */
void Pulse_LongPin_SetInactive(void)
{
    if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
        Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
    else
        Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();
}

void Pulse_nPulseLong_SetPW(float pw, float interval_s)
{
    if (pw <= 0.0f) pw = 0.001f;
    if (interval_s <= 0.0f) interval_s = 0.001f;

    const uint32_t f_clk = 170000000UL;
    const float period_s = pw + interval_s;
    uint32_t target_psc;
    uint32_t target_arr;    /* ARR = target_arr - 1, 单周期 = PW + Interval */
    uint32_t target_ccr;    /* CC1 = PW, 比较匹配点拉低引脚 */

    if (period_s <= 20.0f)
    {
        target_psc = 0;
        target_arr = (uint32_t)(period_s * (float)f_clk);
        target_ccr = (uint32_t)(pw * (float)f_clk);
    }
    else
    {
        target_psc = (f_clk / 1000000UL) - 1UL;   /* 分频后 1MHz 计数频率 */
        target_arr = (uint32_t)(period_s * 1000000.0f);
        target_ccr = (uint32_t)(pw * 1000000.0f);
    }

    if (target_arr == 0) target_arr = 1;
    if (target_ccr == 0) target_ccr = 1;
    if (target_ccr >= target_arr) target_ccr = target_arr - 1;

    __HAL_TIM_SET_PRESCALER(&htim5, target_psc);
    __HAL_TIM_SET_AUTORELOAD(&htim5, target_arr - 1);
    __HAL_TIM_SET_COMPARE(&htim5, TIM_CHANNEL_1, target_ccr);
    __HAL_TIM_SET_COUNTER(&htim5, 0);

    TIM5->EGR = TIM_EGR_UG;
    TIM5->SR &= ~(TIM_SR_UIF | TIM_SR_CC1IF);
}

void Pulse_nPulseLong_Init(void)
{
    g_pulse_ctrl.mode = PULSE_MODE_NPULSE_LONG;
    Pulse_SyncContext();
    s_pulse_remain = 0;

    TIM_ClockConfigTypeDef sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig = {0};
    TIM_OC_InitTypeDef sConfigOC = {0};

    htim5.Instance = TIM5;
    htim5.Init.Prescaler = 0;
    htim5.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim5.Init.Period = 4294967295;
    htim5.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim5.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim5) != HAL_OK)
    {
        Error_Handler();
    }

    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim5, &sClockSourceConfig) != HAL_OK)
    {
        Error_Handler();
    }

    sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim5, &sMasterConfig) != HAL_OK)
    {
        Error_Handler();
    }

    /* 配置 CC1 比较输出 (TIMING 模式, 仅产生比较中断用于拉低长脉冲引脚) */
    if (HAL_TIM_OC_Init(&htim5) != HAL_OK)
    {
        Error_Handler();
    }
    sConfigOC.OCMode = TIM_OCMODE_TIMING;
    sConfigOC.Pulse = 0;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_OC_ConfigChannel(&htim5, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
    {
        Error_Handler();
    }
    __HAL_TIM_ENABLE_OCxPRELOAD(&htim5, TIM_CHANNEL_1);

    /* 将 6 路 HRTIM 输出引脚配置为推挽输出 (Timer C 已剥离为 SYNC/帧标记) */
    HAL_GPIO_DeInit(GPIOB, HRT_CHD1_Pin | HRT_CHD2_Pin);
    HAL_GPIO_DeInit(GPIOA, HRT_CHA1_Pin | HRT_CHA2_Pin | HRT_CHB1_Pin | HRT_CHB2_Pin);

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = HRT_CHD1_Pin | HRT_CHD2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = HRT_CHA1_Pin | HRT_CHA2_Pin | HRT_CHB1_Pin | HRT_CHB2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    Pulse_nPulseLong_SetPW(1.0f, 1.0f);
    Pulse_SetPulsePolarity_High();
}

/* 触发一次 N 长脉冲串: 拉高首脉冲, 启动 TIM5 更新 + 比较中断 */
void Pulse_nPulseLong_OnTrigger(uint32_t count)
{
    if (count < 1) count = 1;
    if (count > 100) count = 100;

    s_pulse_remain = count;
    __HAL_TIM_SET_COUNTER(&htim5, 0);
    Pulse_LongPin_SetActive();    /* 首脉冲有效电平 */

    __HAL_TIM_ENABLE_IT(&htim5, TIM_IT_UPDATE | TIM_IT_CC1);
    __HAL_TIM_ENABLE(&htim5);
}

/* 周期溢出: 结束上一脉冲周期; 若还有剩余脉冲则开始下一脉冲, 否则关闭定时器 */
void Pulse_nPulseLong_OnPeriodElapsed(void)
{
    if (s_pulse_remain > 1)
    {
        s_pulse_remain--;
        Pulse_LongPin_SetActive();   /* 下一脉冲有效电平 */
    }
    else
    {
        s_pulse_remain = 0;
        Pulse_LongPin_SetInactive();
        Pulse_Frame_SetInactive();   /* 帧标记拉低: 一帧猝发结束 */
        __HAL_TIM_DISABLE_IT(&htim5, TIM_IT_UPDATE | TIM_IT_CC1);
        __HAL_TIM_DISABLE(&htim5);
    }
}

/* 比较匹配 (CC1): 当前脉冲宽度到达, 拉低引脚 */
void Pulse_nPulseLong_OnCompareMatch(void)
{
    Pulse_LongPin_SetInactive();
}

/* 长时间 PWM 相关函数 (TIM5 + GPIO 软件模式) */
void Pulse_lPWM_SetPW(float period_s, float duty_cycle_percent)
{
    if (period_s <= 0.0f) period_s = 0.001f;
    if (duty_cycle_percent < 0.0f) duty_cycle_percent = 0.0f;
    if (duty_cycle_percent > 100.0f) duty_cycle_percent = 100.0f;

    const uint32_t f_clk = 170000000;
    uint32_t psc;

    if (period_s <= 20.0f)
    {
        psc = 0;
        lpwm_arr = (uint32_t)(period_s * (float)f_clk);
    }
    else
    {
        psc = (f_clk / 1000000) - 1;
        lpwm_arr = (uint32_t)(period_s * 1000000.0f);
    }

    if (lpwm_arr == 0) lpwm_arr = 1;

    lpwm_ccr = (uint32_t)((float)lpwm_arr * (duty_cycle_percent / 100.0f));

    TIM5->PSC = psc;
    TIM5->ARR = lpwm_arr - 1;
    TIM5->CCR1 = lpwm_ccr;

    if (!(TIM5->CR1 & TIM_CR1_CEN)) {
        TIM5->EGR = TIM_EGR_UG;
        TIM5->SR &= ~(TIM_SR_UIF | TIM_SR_CC1IF);
    }
}

void Pulse_lPWM_Init(void)
{
    g_pulse_ctrl.mode = PULSE_MODE_PWM_LONG;
    Pulse_SyncContext();

    TIM_ClockConfigTypeDef sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig = {0};
    TIM_OC_InitTypeDef sConfigOC = {0};

    htim5.Instance = TIM5;
    htim5.Init.Prescaler = 0;
    htim5.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim5.Init.Period = 4294967295;
    htim5.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim5.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim5) != HAL_OK)
    {
        Error_Handler();
    }

    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim5, &sClockSourceConfig) != HAL_OK)
    {
        Error_Handler();
    }

    sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim5, &sMasterConfig) != HAL_OK)
    {
        Error_Handler();
    }

    if (HAL_TIM_OC_Init(&htim5) != HAL_OK) {
        Error_Handler();
    }

    sConfigOC.OCMode = TIM_OCMODE_TIMING;
    sConfigOC.Pulse = 0;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_OC_ConfigChannel(&htim5, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) {
        Error_Handler();
    }
    __HAL_TIM_ENABLE_OCxPRELOAD(&htim5, TIM_CHANNEL_1);

    /* 将 6 路 HRTIM 输出引脚配置为推挽输出 (Timer C 已剥离为 SYNC/帧标记) */
    HAL_GPIO_DeInit(GPIOB, HRT_CHD1_Pin | HRT_CHD2_Pin);
    HAL_GPIO_DeInit(GPIOA, HRT_CHA1_Pin | HRT_CHA2_Pin | HRT_CHB1_Pin | HRT_CHB2_Pin);

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = HRT_CHD1_Pin | HRT_CHD2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = HRT_CHA1_Pin | HRT_CHA2_Pin | HRT_CHB1_Pin | HRT_CHB2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    Pulse_lPWM_SetPW(1.0f, 50.0f);
    Pulse_SetPulsePolarity_High();
}

/* ==================== 互补 PWM (HRTIM 高精度) ==================== */

/* 互补通道对选择: 同一 HRTIM Timer 的 Tx1(主路/参考) 与 Tx2(互补), 死区硬件生成 */
void Pulse_Select_CompPair(uint8_t pair_idx)
{
    if (pair_idx > 2U) pair_idx = 0U;
    const bool was_enabled = g_pulse_ctrl.is_enabled;
    if (was_enabled) Pulse_Disable_Output();   /* 先关断旧通道对, 严禁打出寄生脉冲 */

    g_pulse_ctrl.mode     = PULSE_MODE;
    g_pulse_ctrl.pair_idx = pair_idx;

    switch (pair_idx)
    {
        case COMP_PAIR_CH1_CH2: /* Timer B: TB1(PA10,CH2)=主路, TB2(PA11,CH1)=互补 */
            g_pulse_ctrl.timer_idx  = HRTIM_TIMERINDEX_TIMER_B;
            g_pulse_ctrl.timer_id   = HRTIM_TIMERID_TIMER_B;
            g_pulse_ctrl.output_ch  = HRTIM_OUTPUT_TB1;
            g_pulse_ctrl.output_ch2 = HRTIM_OUTPUT_TB2;
            break;
        case COMP_PAIR_CH3_CH4: /* Timer A: TA1(PA8,CH4)=主路, TA2(PA9,CH3)=互补 */
            g_pulse_ctrl.timer_idx  = HRTIM_TIMERINDEX_TIMER_A;
            g_pulse_ctrl.timer_id   = HRTIM_TIMERID_TIMER_A;
            g_pulse_ctrl.output_ch  = HRTIM_OUTPUT_TA1;
            g_pulse_ctrl.output_ch2 = HRTIM_OUTPUT_TA2;
            break;
        case COMP_PAIR_CH5_CH6: /* Timer D: TD1(PB14,CH6)=主路, TD2(PB15,CH5)=互补 */
            g_pulse_ctrl.timer_idx  = HRTIM_TIMERINDEX_TIMER_D;
            g_pulse_ctrl.timer_id   = HRTIM_TIMERID_TIMER_D;
            g_pulse_ctrl.output_ch  = HRTIM_OUTPUT_TD1;
            g_pulse_ctrl.output_ch2 = HRTIM_OUTPUT_TD2;
            break;
        default:
            break;
    }

    Pulse_SyncContext();

    if (PULSE_MODE == PULSE_MODE_COMP_PWM)
        Pulse_CompPWM_Init();
    else if (PULSE_MODE == PULSE_MODE_COMP_PWM_LONG)
        Pulse_CompLPWM_Init();

    if (was_enabled) Pulse_Enable_Output();
}

/* 死区时间 -> HRTIM 死区时钟分频档与 9bit 计数值
   fHRTIM = 170MHz, fDTG = fHRTIM 倍频/分频, 单 tick 时间见下表:
     MUL8=0.735ns / MUL4=1.471ns / MUL2=2.941ns / DIV1=5.882ns / DIV2=11.765ns / DIV4=23.529ns
   9bit 值(0~511) 结合 DIV4 档可覆盖至约 12us 死区 */
static void Pulse_CompPWM_CalcDeadTime(uint32_t dt_rise_ns, uint32_t dt_fall_ns,
                                       uint32_t *out_psc, uint32_t *out_rise_val, uint32_t *out_fall_val)
{
    /* 取 rise/fall 较大者决定分频档, 保证 9bit (0~511) 能容纳 */
    uint32_t dt_max = (dt_rise_ns > dt_fall_ns) ? dt_rise_ns : dt_fall_ns;
    uint32_t psc;
    float tick_ns;

    if (dt_max <= 375U)        { psc = HRTIM_TIMDEADTIME_PRESCALERRATIO_MUL8; tick_ns = 0.735294f; }
    else if (dt_max <= 751U)   { psc = HRTIM_TIMDEADTIME_PRESCALERRATIO_MUL4; tick_ns = 1.470588f; }
    else if (dt_max <= 1502U)  { psc = HRTIM_TIMDEADTIME_PRESCALERRATIO_MUL2; tick_ns = 2.941176f; }
    else if (dt_max <= 3006U)  { psc = HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV1; tick_ns = 5.882353f; }
    else if (dt_max <= 6012U)  { psc = HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV2; tick_ns = 11.764706f; }
    else                       { psc = HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV4; tick_ns = 23.529412f; }

    uint32_t rv = (uint32_t)roundf((float)dt_rise_ns / tick_ns);
    uint32_t fv = (uint32_t)roundf((float)dt_fall_ns / tick_ns);
    if (rv > 511U) rv = 511U;
    if (fv > 511U) fv = 511U;

    if (out_psc)      *out_psc      = psc;
    if (out_rise_val) *out_rise_val = rv;
    if (out_fall_val) *out_fall_val = fv;
}

/* 高精度互补 PWM: 周期 1~1500us, 占空比 0~100%, 上升/下降沿死区 0~12000ns */
bool Pulse_CompPWM_SetPW(float period_us, int32_t duty_cycle_percent, uint32_t dt_rise_ns, uint32_t dt_fall_ns)
{
    if (period_us < 1.0f || period_us > 1500.0f)         return false;
    if (duty_cycle_percent < 0 || duty_cycle_percent > 100) return false;
    if (dt_rise_ns > 12000U || dt_fall_ns > 12000U)      return false;

    /* CMP 最小值随分频档变化 (同 PWM, 低于该值 Reset 可能被漏掉) */
    static const uint16_t cmp_min_tab[8] =
        {0x60U, 0x30U, 0x18U, 0x0CU, 0x06U, 0x03U, 0x03U, 0x03U};

    const uint8_t idx         = (uint8_t)g_pulse_ctrl.timer_idx;
    const bool    was_enabled = g_pulse_ctrl.is_enabled;
    bool          must_realign = false;

    uint32_t prescaler_value    = 0U;
    float    current_hrtim_freq = 0.0f;
    uint32_t period_value;
    uint32_t compare_value;
    uint16_t cmp_min;
    HRTIM_TimeBaseCfgTypeDef ScalerCfg = {0};
    HRTIM_CompareCfgTypeDef  CmpCfg    = {0};
    HRTIM_DeadTimeCfgTypeDef DeadTimeCfg = {0};

    if (!Pulse_CalcPrescalerAndCounts(period_us, &prescaler_value, &current_hrtim_freq, NULL))
        return false;

    period_value = (uint32_t)roundf(US_TO_S(period_us) * current_hrtim_freq);
    if (period_value > 0xFFDFU)  period_value = 0xFFDFU;
    else if (period_value < 96U) return false;

    cmp_min = cmp_min_tab[prescaler_value & 0x07U];

    compare_value = (uint32_t)roundf((float)period_value * ((float)duty_cycle_percent / 100.0f));
    if (duty_cycle_percent == 0)
    {
        compare_value = 0U;                        /* 0%: 恒无效电平 */
    }
    else
    {
        if (compare_value < cmp_min)      compare_value = cmp_min;
        if (compare_value >= period_value) compare_value = period_value - 1U;
    }

    /* 死区不得吞没整个脉冲: 钳制在半个周期以内 */
    uint32_t half_period_ns = (uint32_t)((float)period_us * 1000.0f / 2.0f);
    if (half_period_ns == 0U) half_period_ns = 1U;
    if (dt_rise_ns > half_period_ns) dt_rise_ns = half_period_ns;
    if (dt_fall_ns > half_period_ns) dt_fall_ns = half_period_ns;

    /* 确定性重对齐判据 (同 PWM: 换分频档 / PER 收缩 / 停机态均需重对齐) */
    {
        uint32_t cur_psc = hhrtim1.Instance->sTimerxRegs[idx].TIMxCR & HRTIM_TIMCR_CK_PSC;
        uint32_t old_per = hhrtim1.Instance->sTimerxRegs[idx].PERxR  & 0xFFFFU;

        if (prescaler_value != cur_psc) must_realign = true;
        if (period_value    <  old_per) must_realign = true;
        if (!was_enabled)               must_realign = true;
    }

    if (must_realign && was_enabled)
    {
        Pulse_Disable_Output();
    }

    /* 写入新时基与占空比 */
    ScalerCfg.Period            = period_value;
    ScalerCfg.RepetitionCounter = 0;
    ScalerCfg.PrescalerRatio    = prescaler_value;
    ScalerCfg.Mode              = HRTIM_MODE_CONTINUOUS;
    if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, idx, &ScalerCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CmpCfg.CompareValue       = compare_value;
    CmpCfg.AutoDelayedMode    = HRTIM_AUTODELAYEDMODE_REGULAR;
    CmpCfg.AutoDelayedTimeout = 0x0000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, idx, HRTIM_COMPAREUNIT_2, &CmpCfg) != HAL_OK)
    {
        Error_Handler();
    }

    /* 配置死区发生器 (Tx1 参考 / Tx2 互补, 硬件杜绝共态导通) */
    uint32_t dt_psc, dt_rise_val, dt_fall_val;
    Pulse_CompPWM_CalcDeadTime(dt_rise_ns, dt_fall_ns, &dt_psc, &dt_rise_val, &dt_fall_val);

    DeadTimeCfg.Prescaler      = dt_psc;
    DeadTimeCfg.RisingValue    = dt_rise_val;
    DeadTimeCfg.RisingSign     = HRTIM_TIMDEADTIME_RISINGSIGN_POSITIVE;
    DeadTimeCfg.RisingLock     = HRTIM_TIMDEADTIME_RISINGLOCK_WRITE;
    DeadTimeCfg.RisingSignLock = HRTIM_TIMDEADTIME_RISINGSIGNLOCK_WRITE;
    DeadTimeCfg.FallingValue   = dt_fall_val;
    DeadTimeCfg.FallingSign    = HRTIM_TIMDEADTIME_FALLINGSIGN_POSITIVE;
    DeadTimeCfg.FallingLock    = HRTIM_TIMDEADTIME_FALLINGLOCK_WRITE;
    DeadTimeCfg.FallingSignLock = HRTIM_TIMDEADTIME_FALLINGSIGNLOCK_WRITE;
    if (HAL_HRTIM_DeadTimeConfig(&hhrtim1, idx, &DeadTimeCfg) != HAL_OK)
    {
        Error_Handler();
    }

    if (must_realign)
    {
        HAL_HRTIM_SoftwareUpdate(&hhrtim1, idx);
        hhrtim1.Instance->sTimerxRegs[idx].CNTxR = 0U;
    }

    if (must_realign && was_enabled)
    {
        Pulse_Enable_Output();
    }

    return true;
}

void Pulse_CompPWM_Init(void)
{
    g_pulse_ctrl.mode = PULSE_MODE_COMP_PWM;
    Pulse_SyncContext();

    TimeBaseCfg.Period = 65503;
    TimeBaseCfg.RepetitionCounter = 0;
    TimeBaseCfg.PrescalerRatio = HRTIM_PRESCALERRATIO_MUL32;
    TimeBaseCfg.Mode = HRTIM_MODE_CONTINUOUS;
    if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, g_pulse_ctrl.timer_idx, &TimeBaseCfg) != HAL_OK)
    {
        Error_Handler();
    }

    TimerCtl.UpDownMode = HRTIM_TIMERUPDOWNMODE_UP;
    TimerCtl.TrigHalf = HRTIM_TIMERTRIGHALF_DISABLED;
    TimerCtl.GreaterCMP1 = HRTIM_TIMERGTCMP1_EQUAL;
    TimerCtl.DualChannelDacEnable = HRTIM_TIMER_DCDE_DISABLED;
    if (HAL_HRTIM_WaveformTimerControl(&hhrtim1, g_pulse_ctrl.timer_idx, &TimerCtl) != HAL_OK)
    {
        Error_Handler();
    }

    TimerCfg.InterruptRequests = HRTIM_TIM_IT_NONE;
    TimerCfg.DMARequests = HRTIM_TIM_DMA_NONE;
    TimerCfg.DMASrcAddress = 0x0000;
    TimerCfg.DMADstAddress = 0x0000;
    TimerCfg.DMASize = 0x1;
    TimerCfg.HalfModeEnable = HRTIM_HALFMODE_DISABLED;
    TimerCfg.InterleavedMode = HRTIM_INTERLEAVED_MODE_DISABLED;
    TimerCfg.StartOnSync = HRTIM_SYNCSTART_DISABLED;
    TimerCfg.ResetOnSync = HRTIM_SYNCRESET_DISABLED;
    TimerCfg.DACSynchro = HRTIM_DACSYNC_NONE;
    TimerCfg.PreloadEnable = HRTIM_PRELOAD_ENABLED;      /* 开启预装载防抖 */
    TimerCfg.UpdateGating = HRTIM_UPDATEGATING_INDEPENDENT;
    TimerCfg.BurstMode = HRTIM_TIMERBURSTMODE_MAINTAINCLOCK;
    TimerCfg.RepetitionUpdate = HRTIM_UPDATEONREPETITION_DISABLED;
    TimerCfg.PushPull = HRTIM_TIMPUSHPULLMODE_DISABLED;
    TimerCfg.FaultEnable = HRTIM_TIMFAULTENABLE_FAULT2;
    TimerCfg.FaultLock = HRTIM_TIMFAULTLOCK_READWRITE;
    TimerCfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_ENABLED;  /* 开启死区插入 */
    TimerCfg.DelayedProtectionMode = HRTIM_TIMER_A_B_C_DELAYEDPROTECTION_DISABLED;
    TimerCfg.UpdateTrigger = HRTIM_TIMUPDATETRIGGER_NONE;
    TimerCfg.ResetTrigger = HRTIM_TIMRESETTRIGGER_NONE;
    TimerCfg.ResetUpdate = HRTIM_TIMUPDATEONRESET_ENABLED;            /* 复位时更新 */
    TimerCfg.ReSyncUpdate = HRTIM_TIMERESYNC_UPDATE_UNCONDITIONAL;
    if (HAL_HRTIM_WaveformTimerConfig(&hhrtim1, g_pulse_ctrl.timer_idx, &TimerCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CompareCfg.CompareValue = 0;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_1, &CompareCfg) != HAL_OK)
    {
        Error_Handler();
    }

    CompareCfg.CompareValue = 5435;
    CompareCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
    CompareCfg.AutoDelayedTimeout = 0x0000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_COMPAREUNIT_2, &CompareCfg) != HAL_OK)
    {
        Error_Handler();
    }

    /* Tx1 主路输出: CMP1 置位 / CMP2 复位 */
    OutputCfg.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
    OutputCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1;
    OutputCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2;
    OutputCfg.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
    OutputCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    OutputCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    OutputCfg.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    OutputCfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, g_pulse_ctrl.timer_idx, g_pulse_ctrl.output_ch, &OutputCfg) != HAL_OK)
    {
        Error_Handler();
    }

    /* Tx2 互补输出: Set/Reset 均置 NONE, 波形由死区硬件从 Tx1 自动生成 */
    OutputCfg.SetSource = HRTIM_OUTPUTSET_NONE;
    OutputCfg.ResetSource = HRTIM_OUTPUTRESET_NONE;
    if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, g_pulse_ctrl.timer_idx, g_pulse_ctrl.output_ch2, &OutputCfg) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_HRTIM_MspPostInit(&hhrtim1);

    Pulse_CompPWM_SetPW(10.0f, 50, 100, 100);
}

/* ==================== 互补 PWM Long (TIM5 超长, 软件死区) ==================== */

/* 超长互补 PWM: 周期 0.001~1000s, 占空比 0.01~100%, 死区 1~5000ms */
bool Pulse_CompLPWM_SetPW(float period_s, float duty_percent, uint32_t dt_ms)
{
    if (period_s < 0.001f || period_s > 1000.0f) return false;
    if (duty_percent < 0.01f || duty_percent > 100.0f) return false;
    if (dt_ms < 1U || dt_ms > 5000U) return false;

    const uint32_t f_clk = 170000000UL;
    uint32_t psc;
    uint32_t period_ticks;

    if (period_s <= 20.0f)
    {
        psc = 0U;
        period_ticks = (uint32_t)(period_s * (float)f_clk);
    }
    else
    {
        psc = (f_clk / 1000000UL) - 1UL;   /* 分频后 1MHz 计数 */
        period_ticks = (uint32_t)(period_s * 1000000.0f);
    }

    if (period_ticks < 8U) period_ticks = 8U;   /* 至少容纳 3 个比较事件 */
    const uint32_t arr = period_ticks - 1U;

    /* 死区 tick 数 (psc=0: 1ms=170000tick, 否则 1ms=1000tick) */
    uint32_t dt_ticks = (psc == 0U)
        ? (uint32_t)((float)dt_ms * (float)f_clk / 1000.0f)
        : dt_ms * 1000UL;

    /* 死区钳制到半个周期以内, 保证主路与互补路均能导通 */
    uint32_t max_dt = period_ticks / 2U - 1U;
    if (dt_ticks > max_dt) dt_ticks = max_dt;
    if (dt_ticks < 1U)     dt_ticks = 1U;

    /* 占空比点 (主路关断), 钳制保证主路/互补路导通时间均 > 死区 */
    uint32_t duty_ticks = (uint32_t)((float)period_ticks * (duty_percent / 100.0f));
    if (duty_ticks < dt_ticks + 1U)                 duty_ticks = dt_ticks + 1U;
    if (duty_ticks > period_ticks - dt_ticks - 1U)  duty_ticks = period_ticks - dt_ticks - 1U;

    /* 事件时序: CC3(死区点)主路开 -> CC1(占空比)主路关 -> CC2(占空比+死区)互补开 -> 更新(周期末)互补关 */
    const uint32_t cc3 = dt_ticks;                 /* 主路开启 */
    const uint32_t cc1 = duty_ticks;               /* 主路关断 */
    const uint32_t cc2 = duty_ticks + dt_ticks;    /* 互补路开启 */

    __HAL_TIM_SET_PRESCALER(&htim5, psc);
    __HAL_TIM_SET_AUTORELOAD(&htim5, arr);
    __HAL_TIM_SET_COMPARE(&htim5, TIM_CHANNEL_1, cc1);
    __HAL_TIM_SET_COMPARE(&htim5, TIM_CHANNEL_2, cc2);
    __HAL_TIM_SET_COMPARE(&htim5, TIM_CHANNEL_3, cc3);
    __HAL_TIM_SET_COUNTER(&htim5, 0);

    TIM5->EGR = TIM_EGR_UG;
    TIM5->SR &= ~(TIM_SR_UIF | TIM_SR_CC1IF | TIM_SR_CC2IF | TIM_SR_CC3IF);

    return true;
}

void Pulse_CompLPWM_Init(void)
{
    g_pulse_ctrl.mode = PULSE_MODE_COMP_PWM_LONG;
    Pulse_SyncContext();

    TIM_ClockConfigTypeDef sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig = {0};
    TIM_OC_InitTypeDef sConfigOC = {0};

    htim5.Instance = TIM5;
    htim5.Init.Prescaler = 0;
    htim5.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim5.Init.Period = 4294967295;
    htim5.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim5.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim5) != HAL_OK)
    {
        Error_Handler();
    }

    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim5, &sClockSourceConfig) != HAL_OK)
    {
        Error_Handler();
    }

    sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim5, &sMasterConfig) != HAL_OK)
    {
        Error_Handler();
    }

    if (HAL_TIM_OC_Init(&htim5) != HAL_OK)
    {
        Error_Handler();
    }

    /* CC1/CC2/CC3 均为 TIMING 模式, 仅产生比较中断用于软件死区电平切换 */
    sConfigOC.OCMode = TIM_OCMODE_TIMING;
    sConfigOC.Pulse = 0;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_OC_ConfigChannel(&htim5, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_TIM_OC_ConfigChannel(&htim5, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_TIM_OC_ConfigChannel(&htim5, &sConfigOC, TIM_CHANNEL_3) != HAL_OK)
    {
        Error_Handler();
    }
    __HAL_TIM_ENABLE_OCxPRELOAD(&htim5, TIM_CHANNEL_1);
    __HAL_TIM_ENABLE_OCxPRELOAD(&htim5, TIM_CHANNEL_2);
    __HAL_TIM_ENABLE_OCxPRELOAD(&htim5, TIM_CHANNEL_3);

    /* 将 6 路 HRTIM 输出引脚配置为推挽输出 (软件翻转互补电平) */
    HAL_GPIO_DeInit(GPIOB, HRT_CHD1_Pin | HRT_CHD2_Pin);
    HAL_GPIO_DeInit(GPIOA, HRT_CHA1_Pin | HRT_CHA2_Pin | HRT_CHB1_Pin | HRT_CHB2_Pin);

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = HRT_CHD1_Pin | HRT_CHD2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = HRT_CHA1_Pin | HRT_CHA2_Pin | HRT_CHB1_Pin | HRT_CHB2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* 初始电平: 主路与互补路均无效, 严禁直通 */
    Pulse_CompPinMainSetInactive();
    Pulse_CompPinCompSetInactive();

    Pulse_CompLPWM_SetPW(1.0f, 50.0f, 10);
}

/* 周期溢出: 互补路先关断 (主路保持关断, 待 CC3 死区点后开启) */
void Pulse_CompLPWM_OnPeriodElapsed(void)
{
    Pulse_CompPinCompSetInactive();
}

/* CC1 匹配(占空比点): 主路关断 */
void Pulse_CompLPWM_OnDuty(void)
{
    Pulse_CompPinMainSetInactive();
}

/* CC2 匹配(占空比+死区点): 互补路开启 (主路已关断, 死区保证) */
void Pulse_CompLPWM_OnCompOn(void)
{
    Pulse_CompPinCompSetActive();
}

/* CC3 匹配(死区点): 主路开启 (互补路已在周期起点关断) */
void Pulse_CompLPWM_OnMainOn(void)
{
    Pulse_CompPinMainSetActive();
}

/* 获取长脉冲模式对应的 GPIO 端口 (严格对齐 HARDWARE.md §5.1) */
GPIO_TypeDef *Pulse_GetLongPulsePort(void)
{
    switch (g_pulse_ctrl.output_ch)
    {
        case HRTIM_OUTPUT_TB2:
        case HRTIM_OUTPUT_TB1:
        case HRTIM_OUTPUT_TA2:
        case HRTIM_OUTPUT_TA1:
            return GPIOA;

        case HRTIM_OUTPUT_TD2:
        case HRTIM_OUTPUT_TD1:
        case HRTIM_OUTPUT_TC2:
        case HRTIM_OUTPUT_TC1:
            return GPIOB;

        default:
            return GPIOA;
    }
}

/* 获取长脉冲模式对应的 GPIO 引脚 (严格对齐 HARDWARE.md §5.1) */
uint16_t Pulse_GetLongPulsePin(void)
{
    switch (g_pulse_ctrl.output_ch)
    {
        case HRTIM_OUTPUT_TB2:
            return HRT_CHB2_Pin;    /* PA11 -> Y1 */
        case HRTIM_OUTPUT_TB1:
            return HRT_CHB1_Pin;    /* PA10 -> Y2 */
        case HRTIM_OUTPUT_TA2:
            return HRT_CHA2_Pin;    /* PA9  -> Y3 */
        case HRTIM_OUTPUT_TA1:
            return HRT_CHA1_Pin;    /* PA8  -> Y4 */
        case HRTIM_OUTPUT_TD2:
            return HRT_CHD2_Pin;    /* PB15 -> Y5 */
        case HRTIM_OUTPUT_TD1:
            return HRT_CHD1_Pin;    /* PB14 -> Y6 */
        case HRTIM_OUTPUT_TC2:
            return HRT_CHC2_Pin;    /* PB13 -> Y7 */
        case HRTIM_OUTPUT_TC1:
            return HRT_CHC1_Pin;    /* PB12 -> Y8 */
        default:
            return HRT_CHB2_Pin;
    }
}

/* ==================== Y7 SYNC OUT / Y8 帧标记 / Burst PRF ==================== */

/* Y7 帧/猝发标记电平控制 (软件 GPIO, BSRR 原子写) */
void Pulse_Frame_SetActive(void)
{
    FRAME_OUT_GPIO_Port->BSRR = FRAME_OUT_Pin;
}

void Pulse_Frame_SetInactive(void)
{
    FRAME_OUT_GPIO_Port->BSRR = (uint32_t)FRAME_OUT_Pin << 16U;
}

/* Y7 SYNC OUT 初始化: Timer C CH2 (PB13) 配置为单次短脉冲
   每按下一次 TRG, 由下方 Pulse_TriggerFireAll 写入 TCRST 复位 Timer C,
   输出一个宽度 200ns 的同步脉冲, 与首脉冲同一起点 (ns 级对齐) */
void Pulse_Sync_Init(void)
{
    HRTIM_TimeBaseCfgTypeDef SyncTB   = {0};
    HRTIM_TimerCtlTypeDef    SyncTC   = {0};
    HRTIM_TimerCfgTypeDef    SyncCfg  = {0};
    HRTIM_CompareCfgTypeDef  SyncCmp  = {0};
    HRTIM_OutputCfgTypeDef   SyncOut  = {0};

    /* 时基: MUL32 最高分辨率 (5.44GHz), PER = 脉宽 + 余量, 单次模式待 TCRST 触发
       (PER 需 > CMP2, 避免复位沿与周期溢出沿重合导致脉宽异常) */
    SyncTB.Period            = SYNC_OUT_WIDTH_TICKS + 32U;
    SyncTB.RepetitionCounter = 0;
    SyncTB.PrescalerRatio    = HRTIM_PRESCALERRATIO_MUL32;
    SyncTB.Mode              = HRTIM_MODE_SINGLESHOT;
    if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_C, &SyncTB) != HAL_OK)
    {
        Error_Handler();
    }

    SyncTC.UpDownMode = HRTIM_TIMERUPDOWNMODE_UP;
    SyncTC.TrigHalf = HRTIM_TIMERTRIGHALF_DISABLED;
    SyncTC.GreaterCMP1 = HRTIM_TIMERGTCMP1_EQUAL;
    SyncTC.DualChannelDacEnable = HRTIM_TIMER_DCDE_DISABLED;
    if (HAL_HRTIM_WaveformTimerControl(&hhrtim1, HRTIM_TIMERINDEX_TIMER_C, &SyncTC) != HAL_OK)
    {
        Error_Handler();
    }

    SyncCfg.InterruptRequests = HRTIM_TIM_IT_NONE;
    SyncCfg.DMARequests = HRTIM_TIM_DMA_NONE;
    SyncCfg.DMASrcAddress = 0x0000;
    SyncCfg.DMADstAddress = 0x0000;
    SyncCfg.DMASize = 0x1;
    SyncCfg.HalfModeEnable = HRTIM_HALFMODE_DISABLED;
    SyncCfg.InterleavedMode = HRTIM_INTERLEAVED_MODE_DISABLED;
    SyncCfg.StartOnSync = HRTIM_SYNCSTART_DISABLED;
    SyncCfg.ResetOnSync = HRTIM_SYNCRESET_DISABLED;
    SyncCfg.DACSynchro = HRTIM_DACSYNC_NONE;
    SyncCfg.PreloadEnable = HRTIM_PRELOAD_DISABLED;
    SyncCfg.UpdateGating = HRTIM_UPDATEGATING_INDEPENDENT;
    SyncCfg.BurstMode = HRTIM_TIMERBURSTMODE_MAINTAINCLOCK;
    SyncCfg.RepetitionUpdate = HRTIM_UPDATEONREPETITION_DISABLED;
    SyncCfg.PushPull = HRTIM_TIMPUSHPULLMODE_DISABLED;
    SyncCfg.FaultEnable = HRTIM_TIMFAULTENABLE_NONE;
    SyncCfg.FaultLock = HRTIM_TIMFAULTLOCK_READWRITE;
    SyncCfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_DISABLED;
    SyncCfg.DelayedProtectionMode = HRTIM_TIMER_A_B_C_DELAYEDPROTECTION_DISABLED;
    SyncCfg.UpdateTrigger = HRTIM_TIMUPDATETRIGGER_NONE;
    SyncCfg.ResetTrigger = HRTIM_TIMRESETTRIGGER_NONE;
    SyncCfg.ResetUpdate = HRTIM_TIMUPDATEONRESET_DISABLED;
    SyncCfg.ReSyncUpdate = HRTIM_TIMERESYNC_UPDATE_UNCONDITIONAL;
    if (HAL_HRTIM_WaveformTimerConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_C, &SyncCfg) != HAL_OK)
    {
        Error_Handler();
    }

    /* CMP1 = 0 (复位即置位), CMP2 = 200ns (脉宽到达复位) */
    SyncCmp.CompareValue = 0;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_C, HRTIM_COMPAREUNIT_1, &SyncCmp) != HAL_OK)
    {
        Error_Handler();
    }
    SyncCmp.CompareValue = SYNC_OUT_WIDTH_TICKS;
    SyncCmp.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
    SyncCmp.AutoDelayedTimeout = 0x0000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_C, HRTIM_COMPAREUNIT_2, &SyncCmp) != HAL_OK)
    {
        Error_Handler();
    }

    /* 输出 TC2 (PB13/Y7): CMP1 置位 / CMP2 复位, 高有效 */
    SyncOut.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
    SyncOut.SetSource = HRTIM_OUTPUTSET_TIMCMP1;
    SyncOut.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2;
    SyncOut.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
    SyncOut.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    SyncOut.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
    SyncOut.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    SyncOut.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_C, HRTIM_OUTPUT_TC2, &SyncOut) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_HRTIM_MspPostInit(&hhrtim1);   /* 确保 PB13 为 HRTIM_CHC2 复用 */
    HAL_HRTIM_WaveformOutputStart(&hhrtim1, HRTIM_OUTPUT_TC2);
    HAL_HRTIM_WaveformCountStart(&hhrtim1, HRTIM_TIMERID_TIMER_C);
}

/* 触发发波: 同一写操作同步复位「波形定时器」与「SYNC Timer C」, 保证首脉冲与 SYNC ns 级对齐 */
void Pulse_TriggerFireAll(void)
{
    if (!PULSE_OUT_ENABLED) return;

    uint32_t cr2 = HRTIM_CR2_TCRST;   /* SYNC OUT: Timer C 同步复位 */

    switch (g_pulse_ctrl.timer_idx)
    {
        case HRTIM_TIMERINDEX_TIMER_A: cr2 |= HRTIM_CR2_TARST; break;
        case HRTIM_TIMERINDEX_TIMER_B: cr2 |= HRTIM_CR2_TBRST; break;
        case HRTIM_TIMERINDEX_TIMER_D: cr2 |= HRTIM_CR2_TDRST; break;
        default: break;
    }

    HRTIM1->sCommonRegs.CR2 = cr2;
}

/* TIM3 周期猝发时基初始化 (PRF 猝发重复) */
void Pulse_BurstPRF_Init(void)
{
    __HAL_RCC_TIM3_CLK_ENABLE();

    HAL_NVIC_SetPriority(TIM3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM3_IRQn);

    TIM3->CR1  = 0;              /* 默认停止 */
    TIM3->DIER = TIM_DIER_UIE;   /* 仅使能更新中断 */
    TIM3->PSC  = 0;
    TIM3->ARR  = 1699;           /* 默认 100kHz @ 170MHz */
    s_burst_prf_hz = 0;
}

/* 设置猝发重复频率: 0 = 单次触发, 1 ~ 100000 Hz 周期猝发 */
void Pulse_BurstPRF_Set(uint32_t prf_hz)
{
    s_burst_prf_hz = prf_hz;

    if (prf_hz == 0U)
    {
        Pulse_BurstPRF_Stop();
        return;
    }

    if (prf_hz > BURST_PRF_MAX_HZ) prf_hz = BURST_PRF_MAX_HZ;

    /* TIM3 计数时钟 = 170MHz (APB1 分频 1); 16bit ARR 需按频率选择预分频 */
    uint32_t ticks = 170000000UL / prf_hz;
    uint32_t psc   = ticks / 65536UL;
    uint32_t arr   = ticks / (psc + 1UL);
    if (arr == 0UL)   arr = 1UL;
    if (arr > 65535UL) arr = 65535UL;

    TIM3->PSC = (uint16_t)psc;
    TIM3->ARR = (uint16_t)(arr - 1UL);
    TIM3->CNT = 0;
    TIM3->EGR = TIM_EGR_UG;    /* 立即重载 PSC/ARR, 防止运行中改频滞留旧值 */
    TIM3->SR  = (uint32_t)~TIM_IT_UPDATE;

    /* 输出已使能且为 N 脉冲模式: 按新频率立即启动周期猝发 */
    if (PULSE_OUT_ENABLED && PULSE_MODE == PULSE_MODE_NPULSE)
    {
        Pulse_BurstPRF_Start();
    }
}

/* 启动周期猝发 (使能输出时调用): 立即发首帧, 后续帧由 TIM3 更新中断重发 */
void Pulse_BurstPRF_Start(void)
{
    if (s_burst_prf_hz == 0U) return;
    TIM3->CNT = 0;
    TIM3->SR  = (uint32_t)~TIM_IT_UPDATE;
    TIM3->CR1 |= TIM_CR1_CEN;
    Pulse_BurstPRF_OnTick();   /* 立即发首帧 */
}

/* 停止周期猝发 */
void Pulse_BurstPRF_Stop(void)
{
    TIM3->CR1 &= ~TIM_CR1_CEN;
}

/* TIM3 更新中断: 周期重发一帧猝发 (仅 N 脉冲模式) */
void Pulse_BurstPRF_OnTick(void)
{
    if (!PULSE_OUT_ENABLED) return;
    if (PULSE_MODE != PULSE_MODE_NPULSE) return;

    Pulse_Frame_SetActive();
    Pulse_nPulse_OnTrigger(s_npulse_count);
    Pulse_TriggerFireAll();
}

/* ==================== PA15 = HRTIM_FLT2 硬件故障封锁 ==================== */

/* HLRTIM FLT2 故障中断回调: 硬件已把输出 ns 级强制无效,
   这里软件安全网关断 12V 并复位发波状态机 */
void HAL_HRTIM_Fault2Callback(HRTIM_HandleTypeDef *hhrtim)
{
    (void)hhrtim;
    Pulse_EmergencyStop();
    g_fault_flag = true;   /* 通知 main 循环做 UI 收尾 (ISR 内不做弹窗) */
}

/* 初始化硬件故障封锁: PA15(AF13) -> HRTIM1_FLT2, 低有效(内部上拉, 悬空/正常=高=无故障)
   触发后 Timer A/B/D 的输出被 HRTIM 死区级扣到无效电平, 与软件彻底解耦 */
void Pulse_Fault_Init(void)
{
    GPIO_InitTypeDef       gpio = {0};
    HRTIM_FaultCfgTypeDef  fcfg = {0};

    /* PA15 -> HRTIM1_FLT2 (AF13), 内部上拉: 未连接/正常时高电平, 拉低触发故障 */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    gpio.Pin        = GPIO_PIN_15;
    gpio.Mode       = GPIO_MODE_AF_PP;
    gpio.Pull       = GPIO_PULLUP;
    gpio.Speed      = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate  = GPIO_AF13_HRTIM1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* FLT2: 数字输入引脚, 低有效, 轻度滤波防毛刺, 配置可读写 */
    fcfg.Source   = HRTIM_FAULTSOURCE_DIGITALINPUT;
    fcfg.Polarity = HRTIM_FAULTPOLARITY_LOW;
    fcfg.Filter   = HRTIM_FAULTFILTER_2;      /* fHRTIM 采样 N=4, 兼顾抗毛刺与响应速度 */
    fcfg.Lock     = HRTIM_FAULTLOCK_READWRITE;
    if (HAL_HRTIM_FaultConfig(&hhrtim1, HRTIM_FAULT_2, &fcfg) != HAL_OK)
    {
        Error_Handler();
    }

    /* 使能 FLT2 故障通道 */
    HAL_HRTIM_FaultModeCtl(&hhrtim1, HRTIM_FAULT_2, HRTIM_FAULTMODECTL_ENABLED);

    /* 使能 FLT2 中断 (软件安全网) */
    __HAL_HRTIM_ENABLE_IT(&hhrtim1, HRTIM_IT_FLT2);
    HAL_NVIC_SetPriority(HRTIM1_FLT_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(HRTIM1_FLT_IRQn);
}