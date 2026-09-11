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
#include "usart.h"
#include "uart_comm.h"
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
/* 以下三个标志仅 main.c 内部使用 (定时器 ISR 与主循环同属一个编译单元),
 * 加 static 不影响其 volatile 语义 —— volatile 保证不被缓存, static 只改变
 * 链接可见性, 两者相互独立。 */
static volatile bool display_update_flag = 1;
static volatile bool waiting_for_trg_flag = 0;
static volatile bool triggered = 0;
volatile bool g_12v_enable = true;       /* 12V_OUT 手动开关状态 (Setting 页切换, 默认使能);
                                            被 uart_comm.c 与 WouoUI_user.c 引用, 不可加 static */

extern Option n_pulse_option_array[];
extern Option n_pulse_long_option_array[];
extern Option double_pulse_option_array[];
extern Option pwm_option_array[];
extern Option pwm_long_option_array[];
extern Option comp_pwm_option_array[];
extern Option comp_pwm_long_option_array[];
/* 计数器差分 (仅本文件使用) */
static int32_t last_count = 0;
static int32_t current_count = 0;
static int32_t diff = 0;
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
  MX_USART3_UART_Init();
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
  // USART3 接收(SCPI 命令): 提至优先级 2, 高于刷屏(3), 防 2Mbps 下 RXNE 被刷屏中断抢占致 ORE 丢字节
  HAL_NVIC_SetPriority(USART3_IRQn, 2, 0);
  // 刷屏与 DMA 降至最低优先级 3，绝不阻塞发波
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 3, 0);
  HAL_NVIC_SetPriority(SPI1_IRQn, 3, 1);
  HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 3, 2);

  OLED_Init();
  WouoUI_AttachSendBuffFun(OLED_Update_DisplayBuf);

  TestUI_Init();

  /* Y7 SYNC OUT (Timer C CH2) + Y8 帧标记 + PRF 猝发重复时基 (TIM3) 初始化 */
  Pulse_Sync_Init();
  Pulse_BurstPRF_Init();
  Pulse_Fault_Init();    /* PA15 = HRTIM_FLT2 硬件故障封锁 */
  UartComm_Init();       /* PC 通信协议层初始化 (镜像推流 + SCPI + 虚拟按键) */

  // 默认保持 12V 负载开关关断，待供电稳定后再开启
  LOADSW_DISABLE();
  /* USER CODE END 2 */
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* 硬件 Fault 发生后: ISR 只置标志, 此处做 UI 收尾 (关 Enable 按钮 + 弹窗) */
    if (g_fault_flag)
    {
      g_fault_flag = false;
      Pulse_Fault_HandleUI();
    }

    /* 12V_OUT 互锁 (与逻辑): 只有「LTC4421 检测到任一电源有效」且「用户已使能」
     * 两者同时成立时才导通 PA12; 任一不成立立即关断 (安全优先)。
     *
     * 电气极性 (严格对应 HARDWARE.md §5.3):
     *   - LTC_IS_ANY_PWR_VALID() = PC14/PC15 读到 HIGH 即有效 (经 Q6/Q7 倒相后接入);
     *   - LOADSW_ENABLE()        = PA12 输出 HIGH 才导通 (TPS22810 高有效)。
     *   注意这里的极性关系是"电源有效为高、开关使能为高", 不要凭 MAX6818 或
     *   LTC4421 的其它状态脚极性类比推断。
     *
     * 响应速度: 本处是**掉电场景唯一**的切断点, 且只能做到主循环级延迟 (非 ISR 级)。
     * ns 级的波形保护由 HRTIM FLT2 硬件路径 + Pulse_EmergencyStop() 承担, 两者
     * 场景不同: 本处负责"电源本身掉电", 硬件 Fault 负责"外部故障信号拉低 PA15"。
     *
     * g_12v_enable 由 UI (Setting 页) 与 SCPI (12V:ON/OFF) 写入, 故为 volatile。 */
    if (LTC_IS_ANY_PWR_VALID() && g_12v_enable)
    {
      LOADSW_ENABLE();
    }
    else
    {
      LOADSW_DISABLE();
    }

    /* PC 通信处理: 无条件每轮调用 (便宜: 64B 消费上限 + 滚动风暴窗 + 镜像节流),
     * 与显示刷新解耦 —— 屏幕卡住时 SCPI/ACK 仍能响应 */
    UartComm_Proc();

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
        n_pulse_option_array[8].text = (char *)"--->TRIGGERED<---";
      if (PULSE_OUT_ENABLED && triggered && (PULSE_MODE == PULSE_MODE_NPULSE_LONG))
        n_pulse_long_option_array[7].text = (char *)"--->TRIGGERED<---";
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
/* 手动触发发波: 物理 TRG 按键 (HAL_GPIO_EXTI_Callback) 与 SCPI TRIG (uart_comm.c) 共用,
 * 保证两种触发路径走完全相同的逻辑 (含 N 长脉冲的 TIM5 触发与 TRIGGERED 状态显示) */
void Trigger_Pulse(void)
{
    if (PULSE_OUT_ENABLED && PULSE_MODE == PULSE_MODE_NPULSE)
    {
      /* 短脉冲 N 脉冲: 记录本次突发脉冲个数, 首个脉冲由下方 TxRST 触发, 后续由 CMP4 中断重发 */
      Pulse_nPulse_OnTrigger((uint32_t)n_pulse_option_array[4].val);
      Pulse_Frame_SetActive();   /* 帧标记: 猝发开始 */
    }

    if (PULSE_OUT_ENABLED && PULSE_MODE == PULSE_MODE_NPULSE_LONG)
    {
      /* N 长脉冲: 触发一次脉冲串 (剩余脉冲计数 = 用户设置的 Pulse Count) */
      Pulse_nPulseLong_OnTrigger((uint32_t)n_pulse_long_option_array[4].val);
      Pulse_Frame_SetActive();   /* 帧标记: 猝发开始 */
    }

    /* 波形定时器 + SYNC Timer C 同一写操作复位, 保证首脉冲与 SYNC 沿 ns 级对齐。
     * N 长脉冲走 TIM5 (已由 Pulse_nPulseLong_OnTrigger 启动); 此处复位 HRTIM 供
     * 短脉冲/双脉冲使用, 并输出一次 SYNC 同步脉冲 */
    Pulse_TriggerFireAll();

    triggered = 1;    /* 置触发标志: 主循环据此显示 "--->TRIGGERED<---" */
    TIM16->CNT = 0;   /* 复位状态刷新定时器, 立即刷新触发状态文本 */
}

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
      if (n_pulse_option_array[6].val > 0)
      {
        /* Burst 模式 (PRF > 0): 周期猝发自动运行, 不再等待触发 */
        if (PULSE_OUT_ENABLED)
          n_pulse_option_array[8].text = (char *)"--BURST RUNNING--";
        else
          n_pulse_option_array[8].text = (char *)"--OUTPUT DISABLED--";
      }
      else if (PULSE_OUT_ENABLED)
      {
        if (waiting_for_trg_flag)
          n_pulse_option_array[8].text = (char *)"-WAITING FOR TRIG-";
        else
          n_pulse_option_array[8].text = (char *)"                  ";
      }
      else
      {
        n_pulse_option_array[8].text = (char *)"--OUTPUT DISABLED--";
      }
    }
    else if (PULSE_MODE == PULSE_MODE_NPULSE_LONG)
    {
      if (PULSE_OUT_ENABLED)
      {
        if (waiting_for_trg_flag)
          n_pulse_long_option_array[7].text = (char *)"-WAITING FOR TRIG-";
        else
          n_pulse_long_option_array[7].text = (char *)"                  ";
      }
      else
      {
        n_pulse_long_option_array[7].text = (char *)"--OUTPUT DISABLED--";
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
    else if (PULSE_MODE == PULSE_MODE_COMP_PWM)
    {
      if (PULSE_OUT_ENABLED)
        comp_pwm_option_array[7].text = (char *)"--OUTPUT ENABLED--";
      else
        comp_pwm_option_array[7].text = (char *)"--OUTPUT DISABLED--";
    }
    else if (PULSE_MODE == PULSE_MODE_COMP_PWM_LONG)
    {
      if (PULSE_OUT_ENABLED)
        comp_pwm_long_option_array[6].text = (char *)"--OUTPUT ENABLED--";
      else
        comp_pwm_long_option_array[6].text = (char *)"--OUTPUT DISABLED--";
    }
    waiting_for_trg_flag = !waiting_for_trg_flag;
    triggered = 0;
  }
  if(htim->Instance == TIM5)
  {
    if (PULSE_OUT_ENABLED && PULSE_MODE == PULSE_MODE_NPULSE_LONG)
    {
      /* N 长脉冲: 周期溢出, 交由 Pulse 模块维护剩余脉冲计数与引脚电平 */
      Pulse_nPulseLong_OnPeriodElapsed();
    }
    else if (PULSE_OUT_ENABLED && PULSE_MODE == PULSE_MODE_PWM_LONG)
    {
      /* 周期起点：拉高置为有效电平 */
      if (lpwm_ccr > 0)
      {
        if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
          Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();
        else if (PULSE_POLARITY == PULSE_POLARITY_LOW)
          Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
      }
    }
    else if (PULSE_OUT_ENABLED && PULSE_MODE == PULSE_MODE_COMP_PWM_LONG)
    {
      /* 超长互补 PWM: 周期起点先关断互补路, 主路待 CC3 死区点再开启 */
      Pulse_CompLPWM_OnPeriodElapsed();
    }
  }
}

void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM5)
  {
    if (PULSE_MODE == PULSE_MODE_COMP_PWM_LONG)
    {
      /* 超长互补 PWM: CC1=占空比(主路关), CC2=占空比+死区(互补开), CC3=死区(主路开) */
      if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1)
        Pulse_CompLPWM_OnDuty();
      else if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_2)
        Pulse_CompLPWM_OnCompOn();
      else if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_3)
        Pulse_CompLPWM_OnMainOn();
    }
    else if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1)
    {
      if (PULSE_MODE == PULSE_MODE_NPULSE_LONG)
      {
        /* N 长脉冲: 比较匹配(脉宽到达), 拉低引脚 */
        Pulse_nPulseLong_OnCompareMatch();
      }
      else if (PULSE_MODE == PULSE_MODE_PWM_LONG)
      {
        /* 比较匹配点：拉低置为无效电平 */
        if (lpwm_ccr < lpwm_arr)
        {
          if (PULSE_POLARITY == PULSE_POLARITY_HIGH)
            Pulse_GetLongPulsePort()->BSRR = (uint32_t)Pulse_GetLongPulsePin() << 16U;
          else if (PULSE_POLARITY == PULSE_POLARITY_LOW)
            Pulse_GetLongPulsePort()->BSRR = Pulse_GetLongPulsePin();
        }
      }
    }
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if(GPIO_Pin == KEY_TRG_Pin)
  {
    Trigger_Pulse();   /* 物理 TRG 按键与 SCPI TRIG 共用同一触发路径 */
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

void HAL_HRTIM_Compare4EventCallback(HRTIM_HandleTypeDef *hhrtim, uint32_t TimerIdx)
{
  (void)hhrtim;
  (void)TimerIdx;
  /* 短脉冲 N 脉冲: CMP4 周期结束, 维护剩余计数并重发下一脉冲 */
  if (PULSE_MODE == PULSE_MODE_NPULSE)
  {
    Pulse_nPulse_OnPeriodEnd();
  }
}
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
