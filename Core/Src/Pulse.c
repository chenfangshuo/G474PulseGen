#include "Pulse.h"
#include "hrtim.h"
#include "tim.h"
#include <math.h>

/* 全局控制器实体 */
Pulse_Controller_t g_pulse_ctrl = {
    .channel    = CH1,
    .mode       = PULSE_MODE_NPULSE,
    .timer_idx  = HRTIM_TIMERINDEX_TIMER_B,
    .timer_id   = HRTIM_TIMERID_TIMER_B,
    .output_ch  = HRTIM_OUTPUT_TB2,
    .polarity   = PULSE_POLARITY_HIGH,
    .is_enabled = false
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
    PULSE_MODE               = g_pulse_ctrl.mode;
    PULSE_OUT_ENABLED        = g_pulse_ctrl.is_enabled;
    PULSE_POLARITY           = g_pulse_ctrl.polarity;
}

/* 通道选择：严格按照 HARDWARE.md §5.1 表格映射 CH1 ~ CH8 */
void Pulse_Select_Output(uint8_t CHx)
{
    Pulse_SetPulsePolarity_High();
    if (g_pulse_ctrl.is_enabled)
        Pulse_Disable_Output();

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

    if (PULSE_MODE == PULSE_MODE_NPULSE)
        Pulse_nPulse_Init();
    else if (PULSE_MODE == PULSE_MODE_SINGLE_LONG)
        Pulse_slPulse_Init();
    else if (PULSE_MODE == PULSE_MODE_DPULSE)
        Pulse_dPulse_Init();
    else if (PULSE_MODE == PULSE_MODE_PWM)
        Pulse_PWM_Init();
    else if (PULSE_MODE == PULSE_MODE_PWM_LONG)
        Pulse_lPWM_Init();

    if (g_pulse_ctrl.is_enabled)
        Pulse_Enable_Output();
}

void Pulse_Enable_Output(void)
{
    g_pulse_ctrl.is_enabled = true;
    Pulse_SyncContext();

    HAL_HRTIM_WaveformOutputStart(&hhrtim1, g_pulse_ctrl.output_ch);
    HAL_HRTIM_WaveformCountStart(&hhrtim1, g_pulse_ctrl.timer_id);

    if (PULSE_MODE == PULSE_MODE_PWM_LONG)
    {
        __HAL_TIM_SET_COUNTER(&htim5, 0);
        HAL_TIM_Base_Start_IT(&htim5);
        HAL_TIM_OC_Start_IT(&htim5, TIM_CHANNEL_1);
    }
}

void Pulse_Disable_Output(void)
{
    g_pulse_ctrl.is_enabled = false;
    Pulse_SyncContext();

    HAL_HRTIM_WaveformOutputStop(&hhrtim1, g_pulse_ctrl.output_ch);
    HAL_HRTIM_WaveformCountStop(&hhrtim1, g_pulse_ctrl.timer_id);

    if (PULSE_MODE == PULSE_MODE_SINGLE_LONG)
    {
        __HAL_TIM_DISABLE(&htim5);
        if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
            Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
        else if (PULSE_POLARITY == PULSE_POLARITY_LOW)
            Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();
    }
    else if (PULSE_MODE == PULSE_MODE_PWM_LONG)
    {
        HAL_TIM_Base_Stop_IT(&htim5);
        HAL_TIM_OC_Stop_IT(&htim5, TIM_CHANNEL_1);
        if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
            Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
        else if (PULSE_POLARITY == PULSE_POLARITY_LOW)
            Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();
    }
}

void Pulse_SetPulsePolarity_High(void)
{
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
    if (g_pulse_ctrl.is_enabled)
        Pulse_Enable_Output();
}

void Pulse_SetPulsePolarity_Low(void)
{
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
    if (g_pulse_ctrl.is_enabled)
        Pulse_Enable_Output();
}

/* 单脉冲相关函数 */
bool Pulse_nPulse_SetPW(float pw)
{
    if (pw < 0.01f || pw > 1500.0f)
    {
        return false;
    }

    uint32_t prescaler_value;
    uint32_t compare_value;
    HRTIM_TimeBaseCfgTypeDef ScalerCfg = {0};
    HRTIM_CompareCfgTypeDef CmpCfg = {0};

    if (!Pulse_CalcPrescalerAndCounts(pw, &prescaler_value, NULL, &compare_value))
    {
        return false;
    }

    Pulse_Disable_Output();
    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

    ScalerCfg.Period = 65503;
    ScalerCfg.RepetitionCounter = 0;
    ScalerCfg.PrescalerRatio = prescaler_value;
    ScalerCfg.Mode = HRTIM_MODE_SINGLESHOT;
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

    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

    if (g_pulse_ctrl.is_enabled)
        Pulse_Enable_Output();

    return true;
}

void Pulse_nPulse_Init(void)
{
    g_pulse_ctrl.mode = PULSE_MODE_NPULSE;
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
    TimerCfg.PreloadEnable = HRTIM_PRELOAD_DISABLED;
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

    Pulse_nPulse_SetPW(100.0f);
    Pulse_SetPulsePolarity_High();
}

/* 双脉冲相关函数 */
bool Pulse_dPulse_SetPW(int32_t pw1, int32_t interval, int32_t pw2)
{
    if (pw1 < 1 || pw1 > 200 || pw2 < 1 || pw2 > 200 || interval < 1 || interval > 200)
    {
        return false;
    }

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

    Pulse_Disable_Output();
    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

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

    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

    if (g_pulse_ctrl.is_enabled)
        Pulse_Enable_Output();

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
    TimerCfg.PreloadEnable = HRTIM_PRELOAD_DISABLED;
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
    if (period_us < 1.0f || period_us > 1500.0f || duty_cycle_percent < 1 || duty_cycle_percent > 100)
    {
        return false;
    }

    uint32_t prescaler_value;
    float current_hrtim_freq;
    uint32_t period_value;
    uint32_t compare_value;
    HRTIM_TimeBaseCfgTypeDef ScalerCfg = {0};
    HRTIM_CompareCfgTypeDef CmpCfg = {0};
    bool need_reenable = false;

    float duty_cycle_f = (float)duty_cycle_percent / 100.0f;
    float period_s     = US_TO_S(period_us);

    Pulse_CalcPrescalerAndCounts(period_us, &prescaler_value, &current_hrtim_freq, NULL);

    period_value = (uint32_t)roundf(period_s * current_hrtim_freq);
    if (period_value > 0xFFDF)
        period_value = 0xFFDF;
    else if (period_value < 96)
        return false;

    compare_value = (uint32_t)roundf(period_value * duty_cycle_f);
    if (compare_value >= period_value)
        compare_value = period_value - 1;
    else if (compare_value < 1)
        compare_value = 1;

    uint32_t current_psc_reg_val = (hhrtim1.Instance->sTimerxRegs[g_pulse_ctrl.timer_idx].TIMxCR & HRTIM_TIMCR_CK_PSC);
    if ((prescaler_value != current_psc_reg_val) && g_pulse_ctrl.is_enabled)
    {
        Pulse_Disable_Output();
        HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);
        need_reenable = true;
    }

    ScalerCfg.Period = period_value;
    ScalerCfg.RepetitionCounter = 0;
    ScalerCfg.PrescalerRatio = prescaler_value;
    ScalerCfg.Mode = HRTIM_MODE_CONTINUOUS;
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

    HAL_HRTIM_SoftwareUpdate(&hhrtim1, g_pulse_ctrl.timer_idx);

    if (need_reenable)
        Pulse_Enable_Output();

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
    TimerCfg.PreloadEnable = HRTIM_PRELOAD_DISABLED;
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

    Pulse_PWM_SetPW(1.0f, 50);
    Pulse_SetPulsePolarity_High();
}

/* 长时间单脉冲相关函数 (TIM5 + GPIO 软件模式) */
void Pulse_slPulse_SetPW(float pw)
{
    if (pw <= 0.0f) pw = 0.001f;

    uint32_t target_psc = 0;
    uint32_t target_arr = 0;

    if (pw <= 20.0f)
    {
        target_psc = 0;
        target_arr = (uint32_t)(pw * 170000000.0f);
    }
    else
    {
        target_psc = (170000000 / 1000000) - 1;
        target_arr = (uint32_t)(pw * 1000000.0f);
    }

    if (target_arr == 0) target_arr = 1;

    __HAL_TIM_SET_PRESCALER(&htim5, target_psc);
    __HAL_TIM_SET_AUTORELOAD(&htim5, target_arr - 1);
    __HAL_TIM_SET_COUNTER(&htim5, 0);

    TIM5->EGR = TIM_EGR_UG;
    TIM5->SR &= ~TIM_SR_UIF;
}

void Pulse_slPulse_Init(void)
{
    g_pulse_ctrl.mode = PULSE_MODE_SINGLE_LONG;
    Pulse_SyncContext();

    TIM_ClockConfigTypeDef sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig = {0};

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

    Pulse_slPulse_SetPW(1.0f);
    Pulse_SetPulsePolarity_High();
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

    lpwm_ccr = (uint32_t)((float)lpwm_arr * ((100.0f - duty_cycle_percent) / 100.0f));

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