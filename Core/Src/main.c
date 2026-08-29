/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "hrtim.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "oled.h"
#include "WouoUI.h"
#include "WouoUI_user.h"
#include "Key.h"
#include "Pulse.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
volatile bool display_update_flag = 1;
volatile bool waiting_for_trg_flag = 0;
volatile bool triggered = 0;

extern Option single_pulse_option_array[];
extern Option single_pulse_long_option_array[];
extern Option double_pulse_option_array[];
extern Option pwm_option_array[];
extern Option pwm_long_option_array[];
int32_t last_count = 0;
int32_t current_count = 0;
int32_t diff = 0;
// volatile bool OLED_UPDATE_DONE = true;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_HRTIM1_Init();
  MX_TIM2_Init();
  MX_SPI1_Init();
  MX_TIM6_Init();
  MX_TIM7_Init();
  MX_TIM16_Init();
  MX_TIM5_Init();
  /* USER CODE BEGIN 2 */

  HAL_TIM_Base_Start_IT(&htim6); //打开为屏幕定时刷新的定时器中断
  HAL_TIM_Base_Start_IT(&htim7); //打开为按键定时刷新的定时器中断
  HAL_TIM_Base_Start_IT(&htim16); //打开为输出状态刷新的定时器中断
  HAL_TIM_Encoder_Start(&htim2, TIM_CHANNEL_ALL);
  // 触发中断拉到最高优先级 0
  HAL_NVIC_SetPriority(EXTI1_IRQn, 0, 0);
  // 长脉冲软件翻转次之
  HAL_NVIC_SetPriority(TIM5_IRQn, 1, 0);
  // 按键与状态检测
  HAL_NVIC_SetPriority(TIM7_DAC_IRQn, 2, 0);
  HAL_NVIC_SetPriority(TIM1_UP_TIM16_IRQn, 2, 1);
  // 刷屏与 DMA 降至最低优先级 3，绝不阻塞发波
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 3, 0);
  HAL_NVIC_SetPriority(SPI1_IRQn, 3, 1);
  HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 3, 2);

  OLED_Init();
  WouoUI_AttachSendBuffFun(OLED_Update_DisplayBuf);

  TestUI_Init();

  // 默认保持 12V 负载开关关断，待供电稳定后再开启
  LOADSW_DISABLE();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    //当检测到无供电时关闭12V输出并执行QOD (使用 LTC 状态检测宏)
    if (!LTC_IS_ANY_PWR_VALID())
      LOADSW_DISABLE();

    if (Key_Check(K_UP, KEY_DOWN) || Key_Check(K_UP, KEY_REPEAT))
    {
      WOUOUI_MSG_QUE_SEND(msg_up);
    }
    if (Key_Check(K_DOWN, KEY_DOWN) || Key_Check(K_DOWN, KEY_REPEAT))
    {
      WOUOUI_MSG_QUE_SEND(msg_down);
    }
    if (Key_Check(K_LEFT, KEY_DOWN) || Key_Check(K_LEFT, KEY_REPEAT))
    {
      WOUOUI_MSG_QUE_SEND(msg_left);
    }
    if (Key_Check(K_RIGHT, KEY_DOWN) || Key_Check(K_RIGHT, KEY_REPEAT))
    {
      WOUOUI_MSG_QUE_SEND(msg_right);
    }
    if (Key_Check(K_PRESS, KEY_SINGLE) || Key_Check(K_ENC, KEY_SINGLE))
    {
      WOUOUI_MSG_QUE_SEND(msg_click);
    }
    if (Key_Check(K_PRESS, KEY_LONG) || Key_Check(K_ENC, KEY_LONG))
    {
      WOUOUI_MSG_QUE_SEND(msg_return);
    }

    //每秒运行90次
    if (display_update_flag == 1)
    {
      current_count = __HAL_TIM_GET_COUNTER(&htim2);
      diff = current_count - last_count;

      if (diff >= 2)
      {
        // 向前旋转，模拟“向上按键”按下/释放事件
        WOUOUI_MSG_QUE_SEND(msg_left);
        last_count = current_count; // 更新基准值
      }
      else if (diff <= -2)
      {
        // 向后旋转，模拟“向下按键”按下/释放事件
        WOUOUI_MSG_QUE_SEND(msg_right);
        last_count = current_count; // 更新基准值
      }




      //清除屏幕
      // OLED_Clear();
      WouoUI_Proc(10);
      // 使用可以显示中英文混合字符串的函数，中文使用12X12的字体，英文使用7X12的字体
      // OLED_PrintfMix(0, 0,OLED_12X12_FULL,OLED_7X12_HALF,"你好,HI. CFS");

      if (PULSE_OUT_ENABLED && triggered && (PULSE_MODE == PULSE_MODE_NPULSE))
        single_pulse_option_array[5].text = (char *)"--->TRIGGERED<---";
      if (PULSE_OUT_ENABLED && triggered && (PULSE_MODE == PULSE_MODE_SINGLE_LONG))
        single_pulse_long_option_array[5].text = (char *)"--->TRIGGERED<---";
      if (PULSE_OUT_ENABLED && triggered && (PULSE_MODE == PULSE_MODE_DPULSE))
        double_pulse_option_array[7].text = (char *)"--->TRIGGERED<---";

      display_update_flag = 0;
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV6;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if(htim->Instance == TIM6) // 检查对应定时器
  {
    display_update_flag = 1;
  }
  if(htim->Instance == TIM7)
  {
    Key_Tick();
  }
  if(htim->Instance == TIM16)
  {
    if (PULSE_MODE == PULSE_MODE_NPULSE)
    {
      if (PULSE_OUT_ENABLED)
      {
        if (waiting_for_trg_flag)
          single_pulse_option_array[5].text = (char *)"-WAITING FOR TRIG-";
        else
          single_pulse_option_array[5].text = (char *)"                  ";
      }
      else
      {
        single_pulse_option_array[5].text = (char *)"--OUTPUT DISABLED--";
      }
    }
    else if (PULSE_MODE == PULSE_MODE_SINGLE_LONG)
    {
      if (PULSE_OUT_ENABLED)
      {
        if (waiting_for_trg_flag)
          single_pulse_long_option_array[5].text = (char *)"-WAITING FOR TRIG-";
        else
          single_pulse_long_option_array[5].text = (char *)"                  ";
      }
      else
      {
        single_pulse_long_option_array[5].text = (char *)"--OUTPUT DISABLED--";
      }
    }
    else if (PULSE_MODE == PULSE_MODE_DPULSE)
    {
      if (PULSE_OUT_ENABLED)
      {
        if (waiting_for_trg_flag)
          double_pulse_option_array[7].text = (char *)"-WAITING FOR TRIG-";
        else
          double_pulse_option_array[7].text = (char *)"                  ";
      }
      else
      {
        double_pulse_option_array[7].text = (char *)"--OUTPUT DISABLED--";
      }
    }
    else if (PULSE_MODE == PULSE_MODE_PWM)
    {
      if (PULSE_OUT_ENABLED)
        pwm_option_array[6].text = (char *)"--OUTPUT ENABLED--";
      else
        pwm_option_array[6].text = (char *)"--OUTPUT DISABLED--";
    }
    else if (PULSE_MODE == PULSE_MODE_PWM_LONG)
    {
      if (PULSE_OUT_ENABLED)
        pwm_long_option_array[6].text = (char *)"--OUTPUT ENABLED--";
      else
        pwm_long_option_array[6].text = (char *)"--OUTPUT DISABLED--";
    }
    waiting_for_trg_flag = !waiting_for_trg_flag;
    triggered = 0;

    // 毫秒级高频检测 LTC4421 供电，若掉电则毫秒级快速切断 LOADSW
    if (!LTC_IS_ANY_PWR_VALID())
    {
      LOADSW_DISABLE();
    }

    // 软启动状态机 Tick 更新
    Pulse_SoftStart_Update();
  }
  if(htim->Instance == TIM5)
  {
    if (PULSE_OUT_ENABLED && PULSE_MODE == PULSE_MODE_SINGLE_LONG)
    {
      //拉低 GPIO
      if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
        Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
      else if (PULSE_POLARITY == PULSE_POLARITY_LOW)
        Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();

      //停止定时器
      __HAL_TIM_DISABLE(htim);
      __HAL_TIM_DISABLE_IT(htim, TIM_IT_UPDATE);
    }
    else if (PULSE_OUT_ENABLED && PULSE_MODE == PULSE_MODE_PWM_LONG)
    {
      if (lpwm_ccr > 0)
      {
        if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
          Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
        else if (PULSE_POLARITY == PULSE_POLARITY_LOW)
          Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();
      }
    }
  }
}

void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM5 && htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1)
  {
    // 占空比为 100% 时不拉低
    if (lpwm_ccr < lpwm_arr)
    {
      if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
        Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();
      else if (PULSE_POLARITY == PULSE_POLARITY_LOW)
        Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
    }
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if(GPIO_Pin == KEY_TRG_Pin)
  {
    if (HRTIM_TIMERINDEX_TIMER_X == HRTIM_TIMERINDEX_TIMER_B)
      HRTIM1->sCommonRegs.CR2 |= HRTIM_CR2_TBRST;  // 触发 TB 重置事件
    else if (HRTIM_TIMERINDEX_TIMER_X == HRTIM_TIMERINDEX_TIMER_A)
      HRTIM1->sCommonRegs.CR2 |= HRTIM_CR2_TARST;  // 触发 TA 重置事件
    else if (HRTIM_TIMERINDEX_TIMER_X == HRTIM_TIMERINDEX_TIMER_C)
      HRTIM1->sCommonRegs.CR2 |= HRTIM_CR2_TCRST;  // 触发 TC 重置事件
    else if (HRTIM_TIMERINDEX_TIMER_X == HRTIM_TIMERINDEX_TIMER_D)
      HRTIM1->sCommonRegs.CR2 |= HRTIM_CR2_TDRST;  // 触发 TD 重置事件

    if (PULSE_OUT_ENABLED && PULSE_MODE == PULSE_MODE_SINGLE_LONG)
    {
      __HAL_TIM_SET_COUNTER(&htim5, 0);

      //硬件拉高 GPIO (使用 BSRR 寄存器)
      if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
        Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();
      else if (PULSE_POLARITY == PULSE_POLARITY_LOW)
        Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;

      //启动定时器并开启更新中断
      __HAL_TIM_ENABLE_IT(&htim5, TIM_IT_UPDATE);
      __HAL_TIM_ENABLE(&htim5);
    }


    triggered = 1;
    TIM16->CNT = 0;
  }
}

// void HAL_COMP_TriggerCallback(COMP_HandleTypeDef *hcomp)
// {
//   if(hcomp->Instance == COMP1)
//   {
//     triggered = 1;
//     TIM16->CNT = 0;
//
//   }
// }

// void HAL_HRTIM_RepetitionEventCallback(HRTIM_HandleTypeDef *hhrtim, uint32_t TimerIdx)
// {
//   if (PULSE_MODE == PULSE_MODE_NPULSE)
//   {
//     HAL_HRTIM_WaveformOutputStop(&hhrtim1, HRTIM_OUTPUT_TXX);
//     HAL_HRTIM_WaveformCountStop_IT(&hhrtim1, HRTIM_TIMERID_TIMER_X);
//   }
// }
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    OLED_SPI_TxCpltCallback(hspi);
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  Pulse_EmergencyStop(); // 发生异常时硬件级瞬间关断 HRTIM 全部发波与 12V 负载输出
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
