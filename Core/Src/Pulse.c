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

/* 长脉冲 (TIM5 软件模式) 剩余脉冲计数, 由 TIM5 中断维护 */
static volatile uint32_t s_pulse_remain    = 0;

/* 短脉冲 (HRTIM) 当前突发剩余脉冲计数, 由 CMP4 周期结束中断维护 */
static volatile uint32_t s_npulse_remain   = 0;

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

/* 通道选择：严格按照 HARDWARE.md §5.1 表格映射 CH1 ~ CH8 */
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
        case CH7: /* HRTIM1_CHC2 (PB13 -> Y7) */
            g_pulse_ctrl.timer_idx = HRTIM_TIMERINDEX_TIMER_C;
            g_pulse_ctrl.timer_id  = HRTIM_TIMERID_TIMER_C;
            g_pulse_ctrl.output_ch = HRTIM_OUTPUT_TC2;
            break;
        case CH8: /* HRTIM1_CHC1 (PB12 -> Y8) */
            g_pulse_ctrl.timer_idx = HRTIM_TIMERINDEX_TIMER_C;
            g_pulse_ctrl.timer_id  = HRTIM_TIMERID_TIMER_C;
            g_pulse_ctrl.output_ch = HRTIM_OUTPUT_TC1;
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
}

void Pulse_Disable_Output(void)
{
    g_pulse_ctrl.is_enabled = false;
    Pulse_SyncContext();
    s_npulse_remain = 0;   /* 关断输出时中止短脉冲重触发链 */

    /* 关闭 CMP4 周期结束中断 (仅短脉冲多脉冲使用, 其它模式无需) */
    __HAL_HRTIM_TIMER_DISABLE_IT(&hhrtim1, g_pulse_ctrl.timer_idx, HRTIM_TIM_IT_CMP4);

    HAL_HRTIM_WaveformOutputStop(&hhrtim1, g_pulse_ctrl.output_ch);
    HAL_HRTIM_WaveformCountStop(&hhrtim1, g_pulse_ctrl.timer_id);

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
        OutCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
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
        OutCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
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
        OutCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
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
        OutCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
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
   实测稳定约 2us, 故将用户设定的 Interval 减去该值后再写入硬件周期。
   (补偿后最小可实现实际间隔 ≈ 2us, 即该开销本身) */
#define NPULSE_INTERVAL_COMP_US   2.0f
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
    TimerCfg.FaultEnable = HRTIM_TIMFAULTENABLE_NONE;
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
    OutputCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
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
    TimerCfg.FaultEnable = HRTIM_TIMFAULTENABLE_NONE;
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
    OutputCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
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
    TimerCfg.FaultEnable = HRTIM_TIMFAULTENABLE_NONE;
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
    OutputCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
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

    /* 4. 更新内部状态机为故障保护状态 */
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

    /* 将所有 8 路 HRTIM 输出引脚配置为推挽输出 (包含 Timer C: PB12/PB13) */
    HAL_GPIO_DeInit(GPIOB, HRT_CHD1_Pin | HRT_CHD2_Pin | HRT_CHC1_Pin | HRT_CHC2_Pin);
    HAL_GPIO_DeInit(GPIOA, HRT_CHA1_Pin | HRT_CHA2_Pin | HRT_CHB1_Pin | HRT_CHB2_Pin);

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = HRT_CHD1_Pin | HRT_CHD2_Pin | HRT_CHC1_Pin | HRT_CHC2_Pin;
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

    /* 将所有 8 路 HRTIM 输出引脚配置为推挽输出 (包含 Timer C: PB12/PB13) */
    HAL_GPIO_DeInit(GPIOB, HRT_CHD1_Pin | HRT_CHD2_Pin | HRT_CHC1_Pin | HRT_CHC2_Pin);
    HAL_GPIO_DeInit(GPIOA, HRT_CHA1_Pin | HRT_CHA2_Pin | HRT_CHB1_Pin | HRT_CHB2_Pin);

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = HRT_CHD1_Pin | HRT_CHD2_Pin | HRT_CHC1_Pin | HRT_CHC2_Pin;
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