#include <stdbool.h>

#include "stm32g4xx.h"
#include "hrtim.h"
#include "pulse.h"
#include <math.h>

#include "tim.h"

//CH1---TB2
//CH2---TB1
//CH3---TA2
//CH4---TA1
//CH5---TD2
//CH6---TD1

// HRTIM_HandleTypeDef hhrtim1;
extern HRTIM_TimeBaseCfgTypeDef TimeBaseCfg;
extern HRTIM_TimerCtlTypeDef TimerCtl;
extern HRTIM_TimerCfgTypeDef TimerCfg;
extern HRTIM_CompareCfgTypeDef CompareCfg;
extern HRTIM_OutputCfgTypeDef OutputCfg;

extern volatile char HRTIM_TIMERINDEX_TIMER_X;
extern volatile unsigned long HRTIM_TIMERID_TIMER_X;
extern volatile char HRTIM_OUTPUT_TXX;
extern volatile uint8_t PULSE_MODE;
extern volatile bool PULSE_OUT_ENABLED;
extern volatile bool PULSE_POLARITY;
extern volatile uint32_t lpwm_arr;
extern volatile uint32_t lpwm_ccr;


void Pulse_Select_Output(uint8_t CHx)
{
  Pulse_SetPulsePolarity_High();
  if (PULSE_OUT_ENABLED)
    Pulse_Disable_Output();

  if (CHx == 1)
  {
    HRTIM_TIMERINDEX_TIMER_X = HRTIM_TIMERINDEX_TIMER_B;
    HRTIM_TIMERID_TIMER_X = HRTIM_TIMERID_TIMER_B;
    HRTIM_OUTPUT_TXX = HRTIM_OUTPUT_TB2;
  }
  else if (CHx == 2)
  {
    HRTIM_TIMERINDEX_TIMER_X = HRTIM_TIMERINDEX_TIMER_B;
    HRTIM_TIMERID_TIMER_X = HRTIM_TIMERID_TIMER_B;
    HRTIM_OUTPUT_TXX = HRTIM_OUTPUT_TB1;
  }
  else if (CHx == 3)
  {
    HRTIM_TIMERINDEX_TIMER_X = HRTIM_TIMERINDEX_TIMER_A;
    HRTIM_TIMERID_TIMER_X = HRTIM_TIMERID_TIMER_A;
    HRTIM_OUTPUT_TXX = HRTIM_OUTPUT_TA2;
  }
  else if (CHx == 4)
  {
    HRTIM_TIMERINDEX_TIMER_X = HRTIM_TIMERINDEX_TIMER_A;
    HRTIM_TIMERID_TIMER_X = HRTIM_TIMERID_TIMER_A;
    HRTIM_OUTPUT_TXX = HRTIM_OUTPUT_TA1;
  }
  else if (CHx == 5)
  {
    HRTIM_TIMERINDEX_TIMER_X = HRTIM_TIMERINDEX_TIMER_D;
    HRTIM_TIMERID_TIMER_X = HRTIM_TIMERID_TIMER_D;
    HRTIM_OUTPUT_TXX = HRTIM_OUTPUT_TD2;
  }
  else if (CHx == 6)
  {
    HRTIM_TIMERINDEX_TIMER_X = HRTIM_TIMERINDEX_TIMER_D;
    HRTIM_TIMERID_TIMER_X = HRTIM_TIMERID_TIMER_D;
    HRTIM_OUTPUT_TXX = HRTIM_OUTPUT_TD1;
  }

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

  if (PULSE_OUT_ENABLED)
    Pulse_Enable_Output();
}

void Pulse_Enable_Output(void)
{
    HAL_HRTIM_WaveformOutputStart(&hhrtim1, HRTIM_OUTPUT_TXX);
    HAL_HRTIM_WaveformCountStart(&hhrtim1, HRTIM_TIMERID_TIMER_X);

  if (PULSE_MODE == PULSE_MODE_PWM_LONG)
  {
    __HAL_TIM_SET_COUNTER(&htim5, 0);
    // __HAL_TIM_ENABLE_IT(&htim5, TIM_IT_UPDATE);
    // __HAL_TIM_ENABLE(&htim5);
    HAL_TIM_Base_Start_IT(&htim5);
    HAL_TIM_OC_Start_IT(&htim5, TIM_CHANNEL_1);
  }
}

void Pulse_Disable_Output(void)
{
    HAL_HRTIM_WaveformOutputStop(&hhrtim1, HRTIM_OUTPUT_TXX);
    HAL_HRTIM_WaveformCountStop(&hhrtim1, HRTIM_TIMERID_TIMER_X);

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
  PULSE_POLARITY = PULSE_POLARITY_HIGH;
  HRTIM_OutputCfgTypeDef OutCfg = {0};

  Pulse_Disable_Output();
  HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X);

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
    if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_OUTPUT_TXX, &OutCfg) != HAL_OK)
    {
      Error_Handler();
    }
  }
  if (PULSE_MODE == PULSE_MODE_DPULSE)
  {
    OutCfg.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
    OutCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1 | HRTIM_OUTPUTSET_TIMCMP3;
    OutCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2 | HRTIM_OUTPUTRESET_TIMCMP4;
    OutCfg.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
    OutCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    OutCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
    OutCfg.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    OutCfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_OUTPUT_TXX, &OutCfg) != HAL_OK)
    {
      Error_Handler();
    }
  }

  // if (PULSE_MODE == PULSE_MODE_SINGLE_LONG || PULSE_MODE == PULSE_MODE_PWM_LONG)
  //     Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;

  HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X);
  if (PULSE_OUT_ENABLED)
    Pulse_Enable_Output();
}

void Pulse_SetPulsePolarity_Low(void)
{
  PULSE_POLARITY = PULSE_POLARITY_LOW;
  HRTIM_OutputCfgTypeDef OutCfg = {0};

  Pulse_Disable_Output();
  HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X);

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
    if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_OUTPUT_TXX, &OutCfg) != HAL_OK)
    {
      Error_Handler();
    }
  }
  if (PULSE_MODE == PULSE_MODE_DPULSE)
  {
    OutCfg.Polarity = HRTIM_OUTPUTPOLARITY_LOW;
    OutCfg.SetSource = HRTIM_OUTPUTSET_TIMCMP1 | HRTIM_OUTPUTSET_TIMCMP3;
    OutCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP2 | HRTIM_OUTPUTRESET_TIMCMP4;
    OutCfg.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
    OutCfg.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    OutCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
    OutCfg.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    OutCfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_OUTPUT_TXX, &OutCfg) != HAL_OK)
    {
      Error_Handler();
    }
  }

  // if (PULSE_MODE == PULSE_MODE_SINGLE_LONG || PULSE_MODE == PULSE_MODE_PWM_LONG)
  //   Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();

  HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X);
  if (PULSE_OUT_ENABLED)
    Pulse_Enable_Output();
}


//单脉冲相关函数
bool Pulse_nPulse_SetPW(float pw)
{
  // 范围检查：10ns (0.01us) 到 1.5ms (1500us)
  if (pw < 0.01f || pw > 1500.0f)
  {
      return 0; // 脉宽超出范围
  }

  uint32_t prescaler_value;
  float current_hrtim_freq;
  uint32_t compare_value;
  HRTIM_TimeBaseCfgTypeDef ScalerCfg = {0};
  HRTIM_CompareCfgTypeDef CmpCfg = {0};

  // 目标脉宽的秒数
  float pulse_width_s = US_TO_S(pw);

  // --- 动态选择分频器逻辑 ---

  // .------------.------------------.--------.-----------.---------------.--------------.--------------.
  // | CKPSC[2:0] | Prescaling ratio | Scaler | fHRCK/MHz | Resolution/ns | Min freq/KHz | Max Width/us |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    000     |        1         |   32   |   5440    |     0.184     |    83.008    |    12.041    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    001     |        2         |   16   |   2720    |     0.368     |    41.504    |    24.082    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    010     |        4         |   8    |   1360    |     0.735     |    20.752    |    48.164    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    011     |        8         |   4    |    680    |     1.471     |    10.376    |    96.328    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    100     |        16        |   2    |    340    |     2.941     |    5.188     |   192.656    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    101     |        32        |   1    |    170    |     5.882     |    2.594     |   385.312    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    110     |        64        |  0.5   |    85     |    11.765     |    1.297     |   770.624    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    111     |       128        |  0.25  |   42.5    |    23.529     |    0.648     |   1541.247   |
  // '------------'------------------'--------'-----------'---------------'--------------'--------------'


  if (pw <= 11.8f) // 小于等于 11.8 us，使用 x32 倍频以获得最高精度
  {
      prescaler_value = HRTIM_PRESCALERRATIO_MUL32;
      current_hrtim_freq = 170000000.0f * 32.0f;
  }
  else if (pw > 11.8f && pw <= 23.8f)
  {
      prescaler_value = HRTIM_PRESCALERRATIO_MUL16;
      current_hrtim_freq = 170000000.0f * 16.0f;
  }
  else if (pw > 23.8f && pw <= 47.9f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_MUL8;
    current_hrtim_freq = 170000000.0f * 8.0f;
  }
  else if (pw > 47.9f && pw <= 96.0f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_MUL4;
    current_hrtim_freq = 170000000.0f * 4.0f;
  }
  else if (pw > 96.0f && pw <= 192.4f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_MUL2;
    current_hrtim_freq = 170000000.0f * 2.0f;
  }
  else if (pw > 192.4f && pw <= 385.1f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_DIV1;
    current_hrtim_freq = 170000000.0f * 1.0f;
  }
  else if (pw > 385.1f && pw <= 770.4f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_DIV2;
    current_hrtim_freq = 170000000.0f / 2.0f;
  }
  else if (pw > 770.4f && pw <= 1500.0f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_DIV4;
    current_hrtim_freq = 170000000.0f / 4.0f;
  }
  else
  {
    prescaler_value = HRTIM_PRESCALERRATIO_DIV4;
    current_hrtim_freq = 170000000.0f / 4.0f;
  }

  // 计算比较值 (计数值)：Counts = Time * Frequency
  compare_value = (uint32_t)roundf(pulse_width_s * current_hrtim_freq); // 四舍五入

  // 确保计算出的值在16位寄存器的有效范围内
  if (compare_value > 0xFFDF)
  {
      compare_value = 0xFFDF;
  }
  else if (compare_value < 96)
  {
      compare_value = 96; // 最小有效计数值
  }

  //关输出
  Pulse_Disable_Output();
  HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X);

  //设置分频
  ScalerCfg.Period = 65503;
  ScalerCfg.RepetitionCounter = 0;
  ScalerCfg.PrescalerRatio = prescaler_value;
  ScalerCfg.Mode = HRTIM_MODE_SINGLESHOT;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &ScalerCfg) != HAL_OK)
  {
    Error_Handler();
  }


  //设置比较寄存器
  CmpCfg.CompareValue = compare_value;
  CmpCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
  CmpCfg.AutoDelayedTimeout = 0x0000;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_2, &CmpCfg) != HAL_OK)
  {
    Error_Handler();
  }

  //软件更新
  HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X);

  // 重新启动输出
  if (PULSE_OUT_ENABLED)
    Pulse_Enable_Output();

  return 1;
}

void Pulse_nPulse_Init(void)
{
  PULSE_MODE = PULSE_MODE_NPULSE;

  if (HAL_HRTIM_DLLCalibrationStart(&hhrtim1, HRTIM_CALIBRATIONRATE_3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_HRTIM_PollForDLLCalibration(&hhrtim1, 10) != HAL_OK)
  {
    Error_Handler();
  }


  TimeBaseCfg.Period = 65503;
  TimeBaseCfg.RepetitionCounter = 0;
  TimeBaseCfg.PrescalerRatio = HRTIM_PRESCALERRATIO_MUL32;
  TimeBaseCfg.Mode = HRTIM_MODE_SINGLESHOT;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &TimeBaseCfg) != HAL_OK)
  {
    Error_Handler();
  }

  TimerCtl.UpDownMode = HRTIM_TIMERUPDOWNMODE_UP;
  TimerCtl.TrigHalf = HRTIM_TIMERTRIGHALF_DISABLED;
  TimerCtl.GreaterCMP1 = HRTIM_TIMERGTCMP1_EQUAL;
  TimerCtl.DualChannelDacEnable = HRTIM_TIMER_DCDE_DISABLED;
  if (HAL_HRTIM_WaveformTimerControl(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &TimerCtl) != HAL_OK)
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
  if (HAL_HRTIM_WaveformTimerConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &TimerCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CompareCfg.CompareValue = 0;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_1, &CompareCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CompareCfg.CompareValue = 5435;
  CompareCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
  CompareCfg.AutoDelayedTimeout = 0x0000;

  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_2, &CompareCfg) != HAL_OK)
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
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_OUTPUT_TXX, &OutputCfg) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_HRTIM_MspPostInit(&hhrtim1);

  Pulse_nPulse_SetPW(1);
  Pulse_SetPulsePolarity_High();
}


//双脉冲相关函数
bool Pulse_dPulse_SetPW(int32_t pw1, int32_t interval, int32_t pw2)
{
  // 范围检查：1us 到 100us
  if (pw1 < 1 || pw1 > 200 || pw2 < 1 || pw2 > 200 || interval < 1 || interval > 200)
  {
      return 0; // 脉宽超出范围
  }


  float ptotal = (float)(pw1 + interval + pw2);
  uint32_t prescaler_value;
  float current_hrtim_freq;
  uint32_t compare_value2;
  uint32_t compare_value3;
  uint32_t compare_value4;
  HRTIM_TimeBaseCfgTypeDef ScalerCfg = {0};
  HRTIM_CompareCfgTypeDef CmpCfg = {0};

  // 目标脉宽的秒数
  float pw1_s = US_TO_S(pw1);
  float interval_s = US_TO_S(interval);
  float pw2_s = US_TO_S(pw2);

  // --- 动态选择分频器逻辑 ---

  // .------------.------------------.--------.-----------.---------------.--------------.--------------.
  // | CKPSC[2:0] | Prescaling ratio | Scaler | fHRCK/MHz | Resolution/ns | Min freq/KHz | Max Width/us |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    000     |        1         |   32   |   5440    |     0.184     |    83.008    |    12.041    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    001     |        2         |   16   |   2720    |     0.368     |    41.504    |    24.082    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    010     |        4         |   8    |   1360    |     0.735     |    20.752    |    48.164    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    011     |        8         |   4    |    680    |     1.471     |    10.376    |    96.328    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    100     |        16        |   2    |    340    |     2.941     |    5.188     |   192.656    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    101     |        32        |   1    |    170    |     5.882     |    2.594     |   385.312    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    110     |        64        |  0.5   |    85     |    11.765     |    1.297     |   770.624    |
  // :------------+------------------+--------+-----------+---------------+--------------+--------------:
  // |    111     |       128        |  0.25  |   42.5    |    23.529     |    0.648     |   1541.247   |
  // '------------'------------------'--------'-----------'---------------'--------------'--------------'

  if (ptotal <= 11.8f) // 小于等于 11.8 us，使用 x32 倍频以获得最高精度
  {
    prescaler_value = HRTIM_PRESCALERRATIO_MUL32;
    current_hrtim_freq = 170000000.0f * 32.0f;
  }
  else if (ptotal > 11.8f && ptotal <= 23.8f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_MUL16;
    current_hrtim_freq = 170000000.0f * 16.0f;
  }
  else if (ptotal > 23.8f && ptotal <= 47.9f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_MUL8;
    current_hrtim_freq = 170000000.0f * 8.0f;
  }
  else if (ptotal > 47.9f && ptotal <= 96.0f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_MUL4;
    current_hrtim_freq = 170000000.0f * 4.0f;
  }
  else if (ptotal > 96.0f && ptotal <= 192.4f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_MUL2;
    current_hrtim_freq = 170000000.0f * 2.0f;
  }
  else if (ptotal > 192.4f && ptotal <= 385.1f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_DIV1;
    current_hrtim_freq = 170000000.0f * 1.0f;
  }
  else if (ptotal > 385.1f && ptotal <= 770.4f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_DIV2;
    current_hrtim_freq = 170000000.0f / 2.0f;
  }
  else if (ptotal > 770.4f && ptotal <= 1500.0f)
  {
    prescaler_value = HRTIM_PRESCALERRATIO_DIV4;
    current_hrtim_freq = 170000000.0f / 4.0f;
  }
  else
  {
    prescaler_value = HRTIM_PRESCALERRATIO_DIV4;
    current_hrtim_freq = 170000000.0f / 4.0f;
  }

  // 计算比较值 (计数值)：Counts = Time * Frequency
  compare_value2 = (uint32_t)roundf(pw1_s * current_hrtim_freq); // 四舍五入
  compare_value3 = compare_value2 + (uint32_t)roundf(interval_s * current_hrtim_freq);
  compare_value4 = compare_value3 + (uint32_t)roundf(pw2_s * current_hrtim_freq);

  // 确保计算出的值在16位寄存器的有效范围内
  if (compare_value2 > 0xFFDF)
  {
    compare_value2 = 0xFFDF;
  }
  else if (compare_value2 < 96)
  {
    compare_value2 = 96; // 最小有效计数值
  }

  if (compare_value3 > 0xFFDF)
  {
    compare_value3 = 0xFFDF;
  }
  else if (compare_value3 < 96)
  {
    compare_value3 = 96; // 最小有效计数值
  }

  if (compare_value4 > 0xFFDF)
  {
    compare_value4 = 0xFFDF;
  }
  else if (compare_value4 < 96)
  {
    compare_value4 = 96; // 最小有效计数值
  }

  //关输出
  Pulse_Disable_Output();
  HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X);

  //设置分频
  ScalerCfg.Period = 65503;
  ScalerCfg.RepetitionCounter = 0;
  ScalerCfg.PrescalerRatio = prescaler_value;
  ScalerCfg.Mode = HRTIM_MODE_SINGLESHOT;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &ScalerCfg) != HAL_OK)
  {
    Error_Handler();
  }


  //设置比较寄存器
  CmpCfg.CompareValue = 0;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_1, &CmpCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CmpCfg.CompareValue = compare_value2;
  CmpCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
  CmpCfg.AutoDelayedTimeout = 0x0000;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_2, &CmpCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CmpCfg.CompareValue = compare_value3;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_3, &CmpCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CmpCfg.CompareValue = compare_value4;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_4, &CmpCfg) != HAL_OK)
  {
    Error_Handler();
  }



  //软件更新
  HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X);

  // 重新启动输出
  if (PULSE_OUT_ENABLED)
    Pulse_Enable_Output();

  return 1;
}

void Pulse_dPulse_Init()
{
  PULSE_MODE = PULSE_MODE_DPULSE;

  if (HAL_HRTIM_DLLCalibrationStart(&hhrtim1, HRTIM_CALIBRATIONRATE_3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_HRTIM_PollForDLLCalibration(&hhrtim1, 10) != HAL_OK)
  {
    Error_Handler();
  }

  TimeBaseCfg.Period = 65503;
  TimeBaseCfg.RepetitionCounter = 0;
  TimeBaseCfg.PrescalerRatio = HRTIM_PRESCALERRATIO_MUL16;
  TimeBaseCfg.Mode = HRTIM_MODE_SINGLESHOT;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &TimeBaseCfg) != HAL_OK)
  {
    Error_Handler();
  }
  TimerCtl.UpDownMode = HRTIM_TIMERUPDOWNMODE_UP;
  TimerCtl.TrigHalf = HRTIM_TIMERTRIGHALF_DISABLED;
  TimerCtl.GreaterCMP3 = HRTIM_TIMERGTCMP3_EQUAL;
  TimerCtl.GreaterCMP1 = HRTIM_TIMERGTCMP1_EQUAL;
  TimerCtl.DualChannelDacEnable = HRTIM_TIMER_DCDE_DISABLED;
  if (HAL_HRTIM_WaveformTimerControl(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &TimerCtl) != HAL_OK)
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
  if (HAL_HRTIM_WaveformTimerConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &TimerCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CompareCfg.CompareValue = 0;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_1, &CompareCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CompareCfg.CompareValue = 170;
  CompareCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
  CompareCfg.AutoDelayedTimeout = 0x0000;

  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_2, &CompareCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CompareCfg.CompareValue = 340;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_3, &CompareCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CompareCfg.CompareValue = 510;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_4, &CompareCfg) != HAL_OK)
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
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_OUTPUT_TXX, &OutputCfg) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_HRTIM_MspPostInit(&hhrtim1);

  Pulse_dPulse_SetPW(5, 5, 5);
  Pulse_SetPulsePolarity_High();
}


//PWM相关函数
bool Pulse_PWM_SetPW(float period_us, int32_t duty_cycle_percent)
{
    // 范围检查：周期 1us 到 1500us，占空比 1% 到 100%
    if (period_us < 1.0f || period_us > 1500.0f || duty_cycle_percent < 1 || duty_cycle_percent > 100)
    {
        return 0; // 周期或占空比超出范围
    }

    uint32_t prescaler_value;
    float current_hrtim_freq;
    uint32_t period_value;
    uint32_t compare_value;
    HRTIM_TimeBaseCfgTypeDef ScalerCfg = {0};
    HRTIM_CompareCfgTypeDef CmpCfg = {0};
    bool need_reenable = 0;

    // 将百分比占空比转换为浮点数 0.0f - 1.0f
    float duty_cycle_f = (float)duty_cycle_percent / 100.0f;

    // 目标周期的秒数
    float period_s = US_TO_S(period_us);

    // --- 动态选择分频器逻辑 ---
    // 根据目标周期选择一个能覆盖该范围且分辨率尽可能高的分频器
    if (period_us <= 11.8f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_MUL32;
        current_hrtim_freq = 170000000.0f * 32.0f;
    }
    else if (period_us > 11.8f && period_us <= 23.8f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_MUL16;
        current_hrtim_freq = 170000000.0f * 16.0f;
    }
    else if (period_us > 23.8f && period_us <= 47.9f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_MUL8;
        current_hrtim_freq = 170000000.0f * 8.0f;
    }
    else if (period_us > 47.9f && period_us <= 96.0f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_MUL4;
        current_hrtim_freq = 170000000.0f * 4.0f;
    }
    else if (period_us > 96.0f && period_us <= 192.4f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_MUL2;
        current_hrtim_freq = 170000000.0f * 2.0f;
    }
    else if (period_us > 192.4f && period_us <= 385.1f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_DIV1;
        current_hrtim_freq = 170000000.0f * 1.0f;
    }
    else if (period_us > 385.1f && period_us <= 770.4f)
    {
        prescaler_value = HRTIM_PRESCALERRATIO_DIV2;
        current_hrtim_freq = 170000000.0f / 2.0f;
    }
    else // 覆盖 770.4us 到 1500us 的范围
    {
        prescaler_value = HRTIM_PRESCALERRATIO_DIV4;
        current_hrtim_freq = 170000000.0f / 4.0f;
    }

    // 计算周期值 (计数值)：Counts = Time * Frequency
    period_value = (uint32_t)roundf(period_s * current_hrtim_freq);

    // 确保计算出的周期值在16位寄存器的有效范围内（最大 0xFFFF 或 65535）
    if (period_value > 0xFFDF)
    {
        period_value = 0xFFDF;
    }
    // 确保周期至少大于最小比较值，例如最小计数值96
    else if (period_value < 96)
    {
      return 0;
    }

    // 计算比较值 (占空比 * 周期计数值)
    compare_value = (uint32_t)roundf(period_value * duty_cycle_f);

    // 确保比较值在合理范围内 (根据要求，1%到100%范围内)
    if (compare_value >= period_value)
    {
        compare_value = period_value - 1; // 100%占空比 (几乎一直高电平)
    }
    else if (compare_value < 1)
    {
        compare_value = 1; // 1%占空比 (最小有效值，避免0导致一直低电平)
    }


    // 如果分频系数改变，需要先关闭输出并重新配置时基
    // 假设寄存器访问方式与原函数一致
    uint32_t current_psc_reg_val = (hhrtim1.Instance->sTimerxRegs[(uint32_t)HRTIM_TIMERINDEX_TIMER_X].TIMxCR & HRTIM_TIMCR_CK_PSC);
    if ((prescaler_value != current_psc_reg_val) && PULSE_OUT_ENABLED)
    {
      Pulse_Disable_Output();
      HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X);
      need_reenable = 1;
    }

    // 设置分频系数和新的周期值
    ScalerCfg.Period = period_value;
    ScalerCfg.RepetitionCounter = 0;
    ScalerCfg.PrescalerRatio = prescaler_value; // 使用动态计算的值
    ScalerCfg.Mode = HRTIM_MODE_CONTINUOUS;
    if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &ScalerCfg) != HAL_OK)
    {
      Error_Handler();
    }


    // 设置比较寄存器
    CmpCfg.CompareValue = compare_value;
    CmpCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
    CmpCfg.AutoDelayedTimeout = 0x0000;
    if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_2, &CmpCfg) != HAL_OK)
    {
        Error_Handler();
    }

    // 软件更新，使所有更改生效
    HAL_HRTIM_SoftwareUpdate(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X);

    // 重新启动输出（如果之前禁用了）
    if (need_reenable)
        Pulse_Enable_Output();

    return 1;
}

void Pulse_PWM_Init(void)
{
  PULSE_MODE = PULSE_MODE_PWM;

  if (HAL_HRTIM_DLLCalibrationStart(&hhrtim1, HRTIM_CALIBRATIONRATE_3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_HRTIM_PollForDLLCalibration(&hhrtim1, 10) != HAL_OK)
  {
    Error_Handler();
  }


  TimeBaseCfg.Period = 65503;
  TimeBaseCfg.RepetitionCounter = 0;
  TimeBaseCfg.PrescalerRatio = HRTIM_PRESCALERRATIO_MUL32;
  TimeBaseCfg.Mode = HRTIM_MODE_CONTINUOUS;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &TimeBaseCfg) != HAL_OK)
  {
    Error_Handler();
  }

  TimerCtl.UpDownMode = HRTIM_TIMERUPDOWNMODE_UP;
  TimerCtl.TrigHalf = HRTIM_TIMERTRIGHALF_DISABLED;
  TimerCtl.GreaterCMP1 = HRTIM_TIMERGTCMP1_EQUAL;
  TimerCtl.DualChannelDacEnable = HRTIM_TIMER_DCDE_DISABLED;
  if (HAL_HRTIM_WaveformTimerControl(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &TimerCtl) != HAL_OK)
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
  if (HAL_HRTIM_WaveformTimerConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, &TimerCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CompareCfg.CompareValue = 0;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_1, &CompareCfg) != HAL_OK)
  {
    Error_Handler();
  }
  CompareCfg.CompareValue = 5435;
  CompareCfg.AutoDelayedMode = HRTIM_AUTODELAYEDMODE_REGULAR;
  CompareCfg.AutoDelayedTimeout = 0x0000;

  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_COMPAREUNIT_2, &CompareCfg) != HAL_OK)
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
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_X, HRTIM_OUTPUT_TXX, &OutputCfg) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_HRTIM_MspPostInit(&hhrtim1);

  Pulse_PWM_SetPW(1, 50);
  Pulse_SetPulsePolarity_High();
}

//长时间单脉冲相关函数
void Pulse_slPulse_SetPW(float pw)
{
  uint32_t target_psc = 0;
  uint32_t target_arr = 0;

  if (pw <= 20.0f)
  {
    // 高精度量程：170MHz
    target_psc = 0;
    target_arr = (uint32_t)(pw * (float)170000000);
  }
  else
  {
    // 长脉冲量程：分频至 1MHz (170/170)
    target_psc = (170000000 / 1000000) - 1;
    target_arr = (uint32_t)(pw * 1000000.0f);
  }

  __HAL_TIM_SET_PRESCALER(&htim5, target_psc);
  __HAL_TIM_SET_AUTORELOAD(&htim5, target_arr - 1);
  __HAL_TIM_SET_COUNTER(&htim5, 0);

  // 强制更新
  TIM5->EGR = TIM_EGR_UG;
  TIM5->SR &= ~TIM_SR_UIF;
}

void Pulse_slPulse_Init(void)
{
  PULSE_MODE = PULSE_MODE_SINGLE_LONG;

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
  // if (HAL_TIM_OnePulse_Init(&htim5, TIM_OPMODE_SINGLE) != HAL_OK)
  // {
  //   Error_Handler();
  // }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim5, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }

  HAL_GPIO_DeInit(GPIOB, GPIO_PIN_14|GPIO_PIN_15);
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11);

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_14|GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  Pulse_slPulse_SetPW(1.0f);
  Pulse_SetPulsePolarity_High();
}

//长时间PWM相关函数
void Pulse_lPWM_SetPW(float period_s, float duty_cycle_percent)
{
  const uint32_t f_clk = 170000000;
  uint32_t psc;

  //动态分频决策
  if (period_s <= 20.0f)
  {
    psc = 0; // 170MHz
    lpwm_arr = (uint32_t)(period_s * (float)f_clk);
  }
  else
  {
    psc = (f_clk / 1000000) - 1; // 1MHz (PSC=169)
    lpwm_arr = (uint32_t)(period_s * 1000000.0f);
  }

  //计算占空比对应 Tick 数
  lpwm_ccr = (uint32_t)((float)lpwm_arr * ((100.0f - duty_cycle_percent) / 100.0f));

  TIM5->PSC = psc;
  TIM5->ARR = lpwm_arr - 1;
  TIM5->CCR1 = lpwm_ccr;


  //如果已经在运行中，靠定时器自身的溢出自然更新，不要手动重置 CNT
  if (!(TIM5->CR1 & TIM_CR1_CEN)) {
    TIM5->EGR = TIM_EGR_UG;
    TIM5->SR &= ~(TIM_SR_UIF | TIM_SR_CC1IF);
  }
}

void Pulse_lPWM_Init(void)
{
  PULSE_MODE = PULSE_MODE_PWM_LONG;

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

  sConfigOC.OCMode = TIM_OCMODE_TIMING; // 仅作为计时使用，不输出到引脚
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_OC_ConfigChannel(&htim5, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) {
    Error_Handler();
  }
  __HAL_TIM_ENABLE_OCxPRELOAD(&htim5, TIM_CHANNEL_1);

  HAL_GPIO_DeInit(GPIOB, GPIO_PIN_14|GPIO_PIN_15);
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11);

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_14|GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  Pulse_lPWM_SetPW(1.0f, 50.0f);
  Pulse_SetPulsePolarity_High();
}

GPIO_TypeDef *Pulse_GetLongPulsePort(void)
{
  switch (HRTIM_OUTPUT_TXX)
  {
    case HRTIM_OUTPUT_TB2:
    case HRTIM_OUTPUT_TB1:
    case HRTIM_OUTPUT_TA2:
    case HRTIM_OUTPUT_TA1:
      return GPIOA;
    case HRTIM_OUTPUT_TD2:
    case HRTIM_OUTPUT_TD1:
      return GPIOB;
    default: return GPIOA;
  }
}

uint16_t Pulse_GetLongPulsePin(void)
{
  switch (HRTIM_OUTPUT_TXX)
  {
    case HRTIM_OUTPUT_TB2:
      return GPIO_PIN_11;
    case HRTIM_OUTPUT_TB1:
      return GPIO_PIN_10;
    case HRTIM_OUTPUT_TA2:
      return GPIO_PIN_9;
    case HRTIM_OUTPUT_TA1:
      return GPIO_PIN_8;
    case HRTIM_OUTPUT_TD2:
      return GPIO_PIN_15;
    case HRTIM_OUTPUT_TD1:
      return GPIO_PIN_14;
    default: return GPIO_PIN_11;
  }
}