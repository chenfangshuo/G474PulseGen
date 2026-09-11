/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.h
  * @brief   USART3 底层驱动 (PB10=TX / PB11=RX, J5 3.3V TTL 接头)
  *          PC 端 1:1 OLED 镜像同步 + 双模远程控制 (SCPI & 虚拟按键)
  *
  * @note    本文件现由 **CubeMX 生成并维护** (USART3 已纳入 .ioc)。
  *          所有手写内容都写在 USER CODE 区内 —— 这是重新生成时唯一能存活的部分。
  *
  * @note    波特率由 **.ioc 单点管理** (当前 2000000 = 2Mbps)。
  *          PC 端必须配置相同值。历史备注: 2000000 对应 CH9111L 高速模块;
  *          若换用 CH340 低速模块 (12Mbps), 需把 .ioc 里的 BaudRate 改为 460800
  *          (921600 在 CH340 上会丢字节), 重新生成后本文件无需改动。
  ******************************************************************************
  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USART_H__
#define __USART_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern UART_HandleTypeDef huart3;

/* USER CODE BEGIN Private defines */
/* USART3_TX -> DMA1_Channel2 的句柄。CubeMX 也会在 stm32g4xx_it.c 的
 * "External variables" 区声明一份 (供 DMA1_Channel2_IRQHandler 使用),
 * 两处重复声明是合法的; 这里保留是为了让本模块的句柄就近可见。 */
extern DMA_HandleTypeDef hdma_usart3_tx;
/* USER CODE END Private defines */

void MX_USART3_UART_Init(void);

/* USER CODE BEGIN Prototypes */
/* 发送完成接口 UartComm_TxComplete() 的声明在 uart_comm.h ——
 * 本模块 (usart.c/uart_comm.c) 都包含该头文件, 此处不再重复声明。 */
/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */
