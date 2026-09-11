/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.c
  * @brief   USART3 底层驱动 (PB10=TX / PB11=RX, J5 3.3V TTL 接头)
  *          PC 端 1:1 OLED 镜像同步 + 双模远程控制 (SCPI & 虚拟按键)
  *          波特率 2Mbps 8N1, TX 走 DMA1_Channel2, RX 走 RXNE 中断逐字节入队
  * @note    本文件现由 **CubeMX 生成并维护** (USART3 已纳入 .ioc)。
  *          手写内容一律写在 USER CODE 区内 —— 重新生成时只有这些能存活。
  *          波特率 / GPIO 上下拉 / DMA 配置 / NVIC 优先级**全部以 .ioc 为准**:
  *          要改这些参数请在 CubeMX 里改, 改本文件的非 USER CODE 区是无效的
  *          (下次重新生成即被覆盖)。各参数的选择理由记录在下面的
  *          USART3_MspInit 0 / USART3_Init 0 区内。
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "usart.h"

/* USER CODE BEGIN 0 */
/* 本文件底部 USER CODE BEGIN 1 内的 HAL_UART_TxCpltCallback 会调用
 * UartComm_TxComplete(), 其声明在 uart_comm.h。
 * 放在 USER CODE 区是因为 CubeMX 会整体重写上面的 Includes 区。 */
#include "uart_comm.h"
/* USER CODE END 0 */

UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart3_tx;

/* USART3 init function */

void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */
  /* 下面的初始化参数全部由 CubeMX 依据 .ioc 生成。几处需要知道的理由:
   *   - BaudRate = 2000000 (2Mbps): PC 端必须配同一值。历史备注: 2Mbps 对应
   *     CH9111L 高速模块; 若换用 CH340 低速模块, 应在 .ioc 中改为 460800
   *     (921600 在 CH340 上会丢字节), 重新生成即可, 本文件无需改动。
   *   - 8N1 / 无硬件流控 / Oversampling 16: 与上位机协议约定一致。 */
  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 2000000;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

void HAL_UART_MspInit(UART_HandleTypeDef* uartHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(uartHandle->Instance==USART3)
  {
  /* USER CODE BEGIN USART3_MspInit 0 */
  /* 本函数体 (时钟使能 / GPIO / DMA / NVIC) 由 CubeMX 依据 .ioc 生成, 不要直接改。
   * 三处"看起来特殊"的配置其实都来自 .ioc, 理由记录于此备查:
   *   - PB11(RX) 上拉 (GPIO_PULLUP): 串口模块未供电时其 TX 输出悬空, 上拉可避免
   *     浮空噪声被识别成起始位 —— 表现为持续收到垃圾字节甚至误触发;
   *   - USART3 中断的抢占优先级 = 3: 刻意压低, 确保绝不抢占 HRTIM 发波关键路径
   *     (FLT2 硬件故障中断为 (0,0), 见 Pulse_Fault_Init)。
   *     注: 子优先级在 CubeMX 里只能填 0, 因为本工程用 NVIC_PRIORITYGROUP_4
   *     (4 位全给抢占优先级, 0 位给子优先级), 子优先级字段实际未参与仲裁 ——
   *     即 (3,0) 与原手写代码的 (3,3) 在硬件上完全等价, 不是行为变化;
   *   - USART3_TX 走 DMA1_Channel2, 请求号 DMA_REQUEST_USART3_TX (DMAMUX 29)。
   * 要调整以上任一项, 请改 .ioc 后重新生成。 */
  /* USER CODE END USART3_MspInit 0 */

  /** Initializes the peripherals clocks
  */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART3;
    PeriphClkInit.Usart3ClockSelection = RCC_USART3CLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    /* USART3 clock enable */
    __HAL_RCC_USART3_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**USART3 GPIO Configuration
    PB10     ------> USART3_TX
    PB11     ------> USART3_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_11;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* USART3 DMA Init */
    /* USART3_TX Init */
    hdma_usart3_tx.Instance = DMA1_Channel2;
    hdma_usart3_tx.Init.Request = DMA_REQUEST_USART3_TX;
    hdma_usart3_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_usart3_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart3_tx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_usart3_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart3_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart3_tx.Init.Mode = DMA_NORMAL;
    hdma_usart3_tx.Init.Priority = DMA_PRIORITY_LOW;
    if (HAL_DMA_Init(&hdma_usart3_tx) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(uartHandle,hdmatx,hdma_usart3_tx);

    /* USART3 interrupt Init */
    HAL_NVIC_SetPriority(USART3_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
  /* USER CODE BEGIN USART3_MspInit 1 */

  /* USER CODE END USART3_MspInit 1 */
  }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef* uartHandle)
{

  if(uartHandle->Instance==USART3)
  {
  /* USER CODE BEGIN USART3_MspDeInit 0 */

  /* USER CODE END USART3_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART3_CLK_DISABLE();

    /**USART3 GPIO Configuration
    PB10     ------> USART3_TX
    PB11     ------> USART3_RX
    */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_10|GPIO_PIN_11);

    /* USART3 DMA DeInit */
    HAL_DMA_DeInit(uartHandle->hdmatx);

    /* USART3 interrupt Deinit */
    HAL_NVIC_DisableIRQ(USART3_IRQn);
  /* USER CODE BEGIN USART3_MspDeInit 1 */

  /* USER CODE END USART3_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USART3 发送完成回调: DMA 发完一帧后由 HAL 调用, 转发给 uart_comm 释放缓冲 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3)
  {
    UartComm_TxComplete();
  }
}

/* USER CODE END 1 */
