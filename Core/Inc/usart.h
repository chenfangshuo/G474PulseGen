/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.h
  * @brief   USART3 底层驱动 (PB10=TX / PB11=RX, J5 3.3V TTL 接头)
  *          PC 端 1:1 OLED 镜像同步 + 双模远程控制 (SCPI & 虚拟按键)
  * @note    手写自维护文件, 不依赖 CubeMX 重新生成 (工程初始无 USART 配置)
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __USART_H__
#define __USART_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 串口波特率 (PC 端必须选择相同值, 两端一致才能通信):
 * 2000000 = CH9111L 高速模块 (480Mbps, 推荐)
 *  460800 = CH340  低速模块 (12Mbps, 兜底; 921600 会丢字节) */
#define USART3_BAUDRATE   2000000u

extern UART_HandleTypeDef huart3;
extern DMA_HandleTypeDef hdma_usart3_tx;   /* USART3_TX -> DMA1_Channel2 */

void MX_USART3_UART_Init(void);

/* 发送完成接口: 由 uart_comm.c 消费 (双缓冲释放) */
void UartComm_TxComplete(void);

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */