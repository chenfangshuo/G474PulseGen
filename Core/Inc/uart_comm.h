/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    uart_comm.h
  * @brief   PC 通信协议层: OLED 镜像推流 + SCPI 文本控制 + 虚拟按键注入
  *          帧格式: <AA 55 A5> <TYPE> <LEN:LE> <Payload> <CRC16:LE>
  *          T=0x01 BTN / 0x02 CMD / 0x03 PING / 0x10 FRAME / 0x11 RSP / 0x12 ACK
  * @note    手写自维护文件, 置于 main 循环 ~90Hz 周期调用
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef UART_COMM_H
#define UART_COMM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 帧头 (3 字节) */
#define UC_SYNC0        0xAAu
#define UC_SYNC1        0x55u
#define UC_SYNC2        0xA5u

/* 帧类型 */
#define UC_TYPE_BTN     0x01u   /* PC->MCU 虚拟按键, 载荷=1..N 键码 */
#define UC_TYPE_CMD     0x02u   /* PC->MCU SCPI ASCII 命令 */
#define UC_TYPE_PING    0x03u   /* 双向心跳 */
#define UC_TYPE_FRAME   0x10u   /* MCU->PC 屏幕镜像 2048B (原始, 未压缩) */
#define UC_TYPE_RSP     0x11u   /* MCU->PC SCPI 响应 ASCII */
#define UC_TYPE_ACK     0x12u   /* MCU->PC 握手/连接确认 */
#define UC_TYPE_FRAME_RLE 0x13u /* MCU->PC 屏幕镜像 RLE 压缩 (载荷=游程编码, 远小于 2048B) */

/* 虚拟按键码 (独立于 WouoUI 内部 InputMsg) */
#define UC_KEY_UP       0x01u
#define UC_KEY_DOWN     0x02u
#define UC_KEY_LEFT     0x03u
#define UC_KEY_RIGHT    0x04u
#define UC_KEY_ENTER    0x05u   /* msg_click */
#define UC_KEY_BACK     0x06u   /* msg_return */
#define UC_KEY_HOME     0x07u   /* 保留 */
#define UC_KEY_WHEEL_UP   0x08u /* 鼠标滚轮上滚: 固件按当前页面类型智能分发 */
#define UC_KEY_WHEEL_DOWN 0x09u /* 鼠标滚轮下滚: 固件按当前页面类型智能分发 */

/* 镜像帧载荷长度 = OLED_DisplayBuf 大小 */
#define UC_FRAME_LEN    2048u

/* RLE 压缩帧载荷上限 (最坏情况: 单字节游程 + 计数字节交错, 约 2x 原始; 留余量取 4096) */
#define UC_FRAME_RLE_MAX 4096u

/* 完整帧总长上限: 取 RLE 上限与原始帧较大者 + 帧头尾 */
#define UC_TOTAL_MAX    (6u + UC_FRAME_RLE_MAX + 2u)

/* 命令载荷上限 (SCPI 字符串) */
#define UC_CMD_MAX      128u

/* 连接超时: 收到 PC 帧后 2s 无数据回 IDLE (仅暂停镜像推流, 不影响发波与命令) */
#define UC_LINK_TIMEOUT_MS   2000u

/* 与 OLED_driver 解耦: 镜像帧由 OLED_Update_DisplayBuf 在脏帧时调用 */
typedef void (*UartComm_MirrorHook)(void);

/* 连接状态 */
typedef enum {
    UC_IDLE = 0,      /* 未连接: 暂停 FRAME 推流 */
    UC_LINKED = 1     /* 已连接: 收到过 PC 帧, 正常推流 */
} UartComm_LinkState;

/* ---- 供 usart.c / IT 调用 ---- */
void        UartComm_Init(void);
void        UartComm_RxByte(uint8_t byte);          /* USART3 ISR 逐字节入队 */
void        UartComm_OreEvent(void);                /* USART3 ISR ORE 事件 (风暴计数) */
void        UartComm_TxComplete(void);              /* TX DMA 完成, 释放缓冲 */
void        UartComm_Proc(void);                    /* 主循环 ~90Hz: 解析+心跳 */

/* ---- 供 OLED_driver.c 调用 ---- */
void        UartComm_MirrorFrame(const uint8_t (*frame)[128]);   /* 2048B 镜像推流 */

/* ---- 供 main / 其它模块查询 ---- */
UartComm_LinkState UartComm_GetLinkState(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_COMM_H */