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
    .is_enabled           = false,
    .deadtime_rising_val  = 0,
    .deadtime_falling_val = 0,
    .softstart = {
        .state        = SOFTSTART_STATE_IDLE,
        .current_duty = 0.0f,
        .target_duty  = 50.0f,
        .step_duty    = 1.0f,
        .period_us    = 10.0f,
        .auto_loadsw  = true
    }
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

    if (g_pulse_ctrl.mode == PULSE_MODE_INTERLEAVED_PWM)
    {
        /* 启动 Master Timer、Timer A 与 Timer B 双通道波形 */
        HAL_HRTIM_WaveformOutputStart(&hhrtim1, HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TB1);
        HAL_HRTIM_WaveformCountStart(&hhrtim1, HRTIM_TIMERID_MASTER | HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B);
    }
    else
    {
        HAL_HRTIM_WaveformOutputStart(&hhrtim1, g_pulse_ctrl.output_ch);
        HAL_HRTIM_WaveformCountStart(&hhrtim1, g_pulse_ctrl.timer_id);
    }

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

    if (g_pulse_ctrl.mode == PULSE_MODE_INTERLEAVED_PWM)
    {
        HAL_HRTIM_WaveformOutputStop(&hhrtim1, HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TB1);
        HAL_HRTIM_WaveformCountStop(&hhrtim1, HRTIM_TIMERID_MASTER | HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B);
    }
    else
    {
        HAL_HRTIM_WaveformOutputStop(&hhrtim1, g_pulse_ctrl.output_ch);
        HAL_HRTIM_WaveformCountStop(&hhrtim1, g_pulse_ctrl.timer_id);
    }

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
    if (period_us < 1.0f || period_us > 1500.0f || duty_cycle_percent < 0 || duty_cycle_percent > 100)
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

    compare_value = (uint32_t)roundf((float)period_value * duty_cycle_f);
    if (compare_value >= period_value)
        compare_value = period_value - 1;
    else if (compare_value < 1 && duty_cycle_percent > 0)
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
    TimerCfg.PreloadEnable = HRTIM_PRELOAD_ENABLED; // 开启预装载防抖
    TimerCfg.UpdateGating = HRTIM_UPDATEGATING_INDEPENDENT;
    TimerCfg.BurstMode = HRTIM_TIMERBURSTMODE_MAINTAINCLOCK;
    TimerCfg.RepetitionUpdate = HRTIM_UPDATEONREPETITION_DISABLED;
    TimerCfg.PushPull = HRTIM_TIMPUSHPULLMODE_DISABLED;
    TimerCfg.FaultEnable = HRTIM_TIMFAULTENABLE_NONE;
    TimerCfg.FaultLock = HRTIM_TIMFAULTLOCK_READWRITE;
    TimerCfg.DeadTimeInsertion = (g_pulse_ctrl.deadtime_rising_val > 0) ? HRTIM_TIMDEADTIMEINSERTION_ENABLED : HRTIM_TIMDEADTIMEINSERTION_DISABLED;
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

/* Step 3: 配置死区时间发生器 (防上下桥臂直通短路) */
bool Pulse_SetDeadTime(uint8_t timer_idx, uint16_t rising_ns, uint16_t falling_ns)
{
    /* HRTIM fHRCK=5.44GHz, Resolution ≈ 0.184ns. tDT = Value * (1 / 5.44GHz) * 2^PSC */
    /* tDT_ns = Value * 0.18382ns (当 Prescaler=DIV1 时) */
    uint32_t rise_val = (uint32_t)roundf((float)rising_ns / 0.18382f);
    uint32_t fall_val = (uint32_t)roundf((float)falling_ns / 0.18382f);

    if (rise_val > 0x1FF) rise_val = 0x1FF;
    if (fall_val > 0x1FF) fall_val = 0x1FF;

    g_pulse_ctrl.deadtime_rising_val  = (uint16_t)rise_val;
    g_pulse_ctrl.deadtime_falling_val = (uint16_t)fall_val;

    HRTIM_DeadTimeCfgTypeDef dt_cfg = {0};
    dt_cfg.Prescaler       = HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV1;
    dt_cfg.RisingValue     = rise_val;
    dt_cfg.RisingSign      = HRTIM_TIMDEADTIME_RISINGSIGN_POSITIVE;
    dt_cfg.RisingLock      = HRTIM_TIMDEADTIME_RISINGLOCK_WRITE;
    dt_cfg.RisingSignLock  = HRTIM_TIMDEADTIME_RISINGSIGNLOCK_WRITE;
    dt_cfg.FallingValue    = fall_val;
    dt_cfg.FallingSign     = HRTIM_TIMDEADTIME_FALLINGSIGN_POSITIVE;
    dt_cfg.FallingLock     = HRTIM_TIMDEADTIME_FALLINGLOCK_WRITE;
    dt_cfg.FallingSignLock = HRTIM_TIMDEADTIME_FALLINGSIGNLOCK_WRITE;

    if (HAL_HRTIM_DeadTimeConfig(&hhrtim1, timer_idx, &dt_cfg) != HAL_OK)
    {
        return false;
    }

    /* 使能该定时器的死区插入 */
    TimerCfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_ENABLED;
    HAL_HRTIM_WaveformTimerConfig(&hhrtim1, timer_idx, &TimerCfg);

    return true;
}

void Pulse_DisableDeadTime(uint8_t timer_idx)
{
    g_pulse_ctrl.deadtime_rising_val  = 0;
    g_pulse_ctrl.deadtime_falling_val = 0;

    TimerCfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_DISABLED;
    HAL_HRTIM_WaveformTimerConfig(&hhrtim1, timer_idx, &TimerCfg);
}

/* Step 4: 180° 交错 PWM (Master Timer 同步移相) */
void Pulse_InterleavedPWM_Init(float period_us, float initial_duty)
{
    g_pulse_ctrl.mode = PULSE_MODE_INTERLEAVED_PWM;
    Pulse_SyncContext();

    uint32_t prescaler_value;
    float current_hrtim_freq;
    uint32_t period_counts;

    Pulse_CalcPrescalerAndCounts(period_us, &prescaler_value, &current_hrtim_freq, NULL);
    period_counts = (uint32_t)roundf(US_TO_S(period_us) * current_hrtim_freq);
    if (period_counts > 0xFFDF) period_counts = 0xFFDF;
    else if (period_counts < 96) period_counts = 96;

    /* 1. 配置 Master Timer 作为全局 180° 移相同步基准 */
    HRTIM_TimeBaseCfgTypeDef MasterTimeBase = {0};
    MasterTimeBase.Period = period_counts;
    MasterTimeBase.RepetitionCounter = 0;
    MasterTimeBase.PrescalerRatio = prescaler_value;
    MasterTimeBase.Mode = HRTIM_MODE_CONTINUOUS;
    HAL_HRTIM_TimeBaseConfig(&hhrtim1, HRTIM_TIMERINDEX_MASTER, &MasterTimeBase);

    /* Master CMP1 触发 TA 同步 (0°), Master CMP2 触发 TB 同步 (180° 处复位) */
    HRTIM_CompareCfgTypeDef MasterCmp = {0};
    MasterCmp.CompareValue = 0;
    HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_MASTER, HRTIM_COMPAREUNIT_1, &MasterCmp);

    MasterCmp.CompareValue = period_counts / 2; // 180 度对称移相比较点
    HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_MASTER, HRTIM_COMPAREUNIT_2, &MasterCmp);

    /* 2. 配置 从定时器 Timer A (Phase 0°) 与 Timer B (Phase 180°) */
    HRTIM_TimeBaseCfgTypeDef SlaveTimeBase = {0};
    SlaveTimeBase.Period = period_counts;
    SlaveTimeBase.RepetitionCounter = 0;
    SlaveTimeBase.PrescalerRatio = prescaler_value;
    SlaveTimeBase.Mode = HRTIM_MODE_CONTINUOUS;

    HAL_HRTIM_TimeBaseConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A, &SlaveTimeBase);
    HAL_HRTIM_TimeBaseConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B, &SlaveTimeBase);

    /* Timer A 接收 Master 同步复位，Timer B 接收 Master CMP2 比较复位事件 */
    HRTIM_TimerCfgTypeDef SlaveTimerCfg = {0};
    SlaveTimerCfg.PreloadEnable = HRTIM_PRELOAD_ENABLED;
    SlaveTimerCfg.BurstMode = HRTIM_TIMERBURSTMODE_MAINTAINCLOCK;
    SlaveTimerCfg.ResetTrigger = HRTIM_TIMRESETTRIGGER_MASTER_CMP1;
    SlaveTimerCfg.ResetUpdate = HRTIM_TIMUPDATEONRESET_ENABLED;
    HAL_HRTIM_WaveformTimerConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A, &SlaveTimerCfg);

    SlaveTimerCfg.ResetTrigger = HRTIM_TIMRESETTRIGGER_MASTER_CMP2;
    HAL_HRTIM_WaveformTimerConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B, &SlaveTimerCfg);

    /* 3. 配置波形输出 (TA1 -> Y4, TB1 -> Y2) */
    HRTIM_OutputCfgTypeDef OutCfg = {0};
    OutCfg.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
    OutCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1;
    OutCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2;
    OutCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A, HRTIM_OUTPUT_TA1, &OutCfg);
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B, HRTIM_OUTPUT_TB1, &OutCfg);

    HAL_HRTIM_MspPostInit(&hhrtim1);

    Pulse_InterleavedPWM_SetPW(period_us, initial_duty);
}

bool Pulse_InterleavedPWM_SetPW(float period_us, float duty_percent)
{
    if (period_us < 1.0f || period_us > 1500.0f || duty_percent < 0.0f || duty_percent > 100.0f)
    {
        return false;
    }

    uint32_t prescaler_value;
    float current_hrtim_freq;
    uint32_t period_counts;

    Pulse_CalcPrescalerAndCounts(period_us, &prescaler_value, &current_hrtim_freq, NULL);
    period_counts = (uint32_t)roundf(US_TO_S(period_us) * current_hrtim_freq);
    if (period_counts > 0xFFDF) period_counts = 0xFFDF;
    else if (period_counts < 96) return false;

    uint32_t cmp_duty = (uint32_t)roundf((float)period_counts * (duty_percent / 100.0f));
    if (cmp_duty >= period_counts) cmp_duty = period_counts - 1;

    HRTIM_CompareCfgTypeDef cmp_cfg = {0};
    cmp_cfg.CompareValue = 0;
    HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A, HRTIM_COMPAREUNIT_1, &cmp_cfg);
    HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B, HRTIM_COMPAREUNIT_1, &cmp_cfg);

    cmp_cfg.CompareValue = cmp_duty;
    HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A, HRTIM_COMPAREUNIT_2, &cmp_cfg);
    HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B, HRTIM_COMPAREUNIT_2, &cmp_cfg);

    HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_MASTER | HRTIM_TIMERINDEX_TIMER_A | HRTIM_TIMERINDEX_TIMER_B);

    return true;
}

/* Step 4: 非阻塞软启动控制 */
void Pulse_SoftStart_Start(float target_duty, uint32_t ramp_time_ms, bool enable_loadsw)
{
    if (ramp_time_ms < 10) ramp_time_ms = 10;
    uint32_t total_ticks = ramp_time_ms / 10; // 10ms 每周期

    g_pulse_ctrl.softstart.current_duty = 0.0f;
    g_pulse_ctrl.softstart.target_duty  = target_duty;
    g_pulse_ctrl.softstart.step_duty    = target_duty / (float)total_ticks;
    g_pulse_ctrl.softstart.auto_loadsw  = enable_loadsw;
    g_pulse_ctrl.softstart.state        = SOFTSTART_STATE_RAMPING;

    /* 先以 0% 占空比启动输出 */
    if (g_pulse_ctrl.mode == PULSE_MODE_INTERLEAVED_PWM)
    {
        Pulse_InterleavedPWM_SetPW(g_pulse_ctrl.softstart.period_us, 0.0f);
    }
    else
    {
        Pulse_PWM_SetPW(g_pulse_ctrl.softstart.period_us, 0);
    }

    Pulse_Enable_Output();
}

void Pulse_SoftStart_Stop(uint32_t ramp_time_ms)
{
    if (ramp_time_ms < 10)
    {
        Pulse_Disable_Output();
        g_pulse_ctrl.softstart.state = SOFTSTART_STATE_IDLE;
        return;
    }

    uint32_t total_ticks = ramp_time_ms / 10;
    g_pulse_ctrl.softstart.step_duty = g_pulse_ctrl.softstart.current_duty / (float)total_ticks;
    g_pulse_ctrl.softstart.state     = SOFTSTART_STATE_STOPPING;
}

/* 软启动定时更新节拍 (应放置在 10ms 定时器中断如 TIM16 中调用) */
void Pulse_SoftStart_Update(void)
{
    if (g_pulse_ctrl.softstart.state == SOFTSTART_STATE_RAMPING)
    {
        g_pulse_ctrl.softstart.current_duty += g_pulse_ctrl.softstart.step_duty;
        if (g_pulse_ctrl.softstart.current_duty >= g_pulse_ctrl.softstart.target_duty)
        {
            g_pulse_ctrl.softstart.current_duty = g_pulse_ctrl.softstart.target_duty;
            g_pulse_ctrl.softstart.state = SOFTSTART_STATE_RUNNING;

            if (g_pulse_ctrl.softstart.auto_loadsw && LTC_IS_ANY_PWR_VALID())
            {
                LOADSW_ENABLE(); // 软启动爬坡完成且供电稳定后安全打开 12V 负载开关
            }
        }

        if (g_pulse_ctrl.mode == PULSE_MODE_INTERLEAVED_PWM)
            Pulse_InterleavedPWM_SetPW(g_pulse_ctrl.softstart.period_us, g_pulse_ctrl.softstart.current_duty);
        else
            Pulse_PWM_SetPW(g_pulse_ctrl.softstart.period_us, (int32_t)g_pulse_ctrl.softstart.current_duty);
    }
    else if (g_pulse_ctrl.softstart.state == SOFTSTART_STATE_STOPPING)
    {
        g_pulse_ctrl.softstart.current_duty -= g_pulse_ctrl.softstart.step_duty;
        if (g_pulse_ctrl.softstart.current_duty <= 0.0f)
        {
            g_pulse_ctrl.softstart.current_duty = 0.0f;
            g_pulse_ctrl.softstart.state = SOFTSTART_STATE_IDLE;
            Pulse_Disable_Output();
            LOADSW_DISABLE();
        }

        if (g_pulse_ctrl.mode == PULSE_MODE_INTERLEAVED_PWM)
            Pulse_InterleavedPWM_SetPW(g_pulse_ctrl.softstart.period_us, g_pulse_ctrl.softstart.current_duty);
        else
            Pulse_PWM_SetPW(g_pulse_ctrl.softstart.period_us, (int32_t)g_pulse_ctrl.softstart.current_duty);
    }
}

/* Step 5: 硬件级与软件级紧急关断 (Safe-State 硬件保护) */
void Pulse_EmergencyStop(void)
{
    /* 1. 瞬时强制拉低 PA12 (LOADSW) 切断 12V 外部供电并启动 QOD 快速放电 */
    LOADSW_DISABLE();

    /* 2. 硬件级清零 HRTIM 输出使能寄存器 (ODISR) 立即断开所有 8 路高精发波输出 */
    HRTIM1->sCommonRegs.ODISR = 0xFFFFFFFFU; // 强制禁用全部通道输出
    HRTIM1->sMasterRegs.MCR  &= ~(HRTIM_MCR_MCEN | HRTIM_MCR_TACEN | HRTIM_MCR_TBCEN | HRTIM_MCR_TCCEN | HRTIM_MCR_TDCEN);

    /* 3. 关闭长脉冲定时器 TIM5 并拉低 GPIO */
    __HAL_TIM_DISABLE(&htim5);
    Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;

    /* 4. 更新内部状态机为故障保护状态 */
    g_pulse_ctrl.is_enabled      = false;
    g_pulse_ctrl.softstart.state = SOFTSTART_STATE_FAULT;
    Pulse_SyncContext();
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