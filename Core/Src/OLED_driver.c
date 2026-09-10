/*
 * 本文件改编自 B 站「江协科技」的开源 STM32 教学库, 在此致谢原作者的无私分享。
 *     官方网站: https://jiangxiekeji.com/
 *     原作品公开发布供下载, 但未声明明确的开源许可, 此处保留署名以尊重原作者。
 *
 * 本项目作者编写与修改的部分, 按 MIT 许可证发布, 详见根目录 LICENSE 文件。
 */
/*
 * 这个头文件是oled库的 [硬件层] 实现文件
 */
#include "stm32g4xx.h"
#include "OLED_driver.h"
#include "spi.h"
#include "uart_comm.h"   /* PC 镜像推流: UartComm_MirrorFrame */

uint8_t OLED_DisplayBuf[128/8][128];
bool OLED_ColorMode = true;

/* 纯异步 DMA 传输控制变量 */
static volatile bool s_oled_dma_busy = false;
static volatile uint8_t s_oled_current_page = 0;
static volatile bool s_oled_updating = false;

/* 静态帧脏标记对比缓冲区与脏标记 */
static uint8_t s_oled_last_frame[128/8][128] = {{0}};
static volatile bool s_oled_dirty = true;

/* SPI 阻塞发送命令与单字节配置（仅在初始化阶段使用） */
static void OLED_Write_CMD_Blocking(uint8_t cmd)
{
    OLED_DC_Clr();
    OLED_CS_Clr();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, 100);
    OLED_CS_Set();
}

static void OLED_Write_DATA_Blocking(uint8_t data)
{
    OLED_DC_Set();
    OLED_CS_Clr();
    HAL_SPI_Transmit(&hspi1, &data, 1, 100);
    OLED_CS_Set();
}

/* 异步写命令 */
void OLED_Write_CMD(uint8_t data)
{
    OLED_Write_CMD_Blocking(data);
}

/* 异步写数据 */
void OLED_Write_DATA(uint8_t data)
{
    OLED_Write_DATA_Blocking(data);
}

void OLED_WriteDataArr(uint8_t *Data, uint8_t Count)
{
    OLED_DC_Set();
    OLED_CS_Clr();
    HAL_SPI_Transmit(&hspi1, Data, Count, 100);
    OLED_CS_Set();
}

/* 反显函数 */
void OLED_ColorTurn(uint8_t i)
{
    if (i == 0)
    {
        OLED_Write_CMD(0xA6); // 正常显示
    }
    else if (i == 1)
    {
        OLED_Write_CMD(0xA7); // 反色显示
    }
}

/* 开启OLED显示 */
void OLED_DisPlay_On(void)
{
    OLED_Write_CMD(0x8D); // 电荷泵使能
    OLED_Write_CMD(0x14); // 开启电荷泵
    OLED_Write_CMD(0xAF); // 点亮屏幕
}

/* 关闭OLED显示 */
void OLED_DisPlay_Off(void)
{
    OLED_Write_CMD(0x8D); // 电荷泵使能
    OLED_Write_CMD(0x10); // 关闭电荷泵
    OLED_Write_CMD(0xAE); // 关闭屏幕
}

/* 设置光标位置 (阻塞式发送3字节命令) */
void OLED_SetCursor(uint8_t Page, uint8_t X)
{
    uint8_t cmd[3];
    cmd[0] = 0xB0 | (Page & 0x0F);
    cmd[1] = 0x10 | ((X & 0xF0) >> 4);
    cmd[2] = 0x00 | (X & 0x0F);

    OLED_DC_Clr();
    OLED_CS_Clr();
    HAL_SPI_Transmit(&hspi1, cmd, 3, 10);
    OLED_CS_Set();
}

static void OLED_SetCursor_ISR(uint8_t page, uint8_t x)
{
    const uint8_t c[3] = {(uint8_t)(0xB0 | (page & 0x0F)),
                          (uint8_t)(0x10 | ((x & 0xF0) >> 4)),
                          (uint8_t)(0x00 |  (x & 0x0F))};
    OLED_DC_Clr(); OLED_CS_Clr();
    for (uint8_t i = 0; i < 3; i++) {
        uint32_t g = 0;
        while (!(SPI1->SR & SPI_SR_TXE)) { if (++g > 20000u) goto abort; }   /* 有界，绝不死等 */
        *(volatile uint8_t *)&SPI1->DR = c[i];
    }
    { uint32_t g = 0; while (SPI1->SR & SPI_SR_BSY) { if (++g > 20000u) break; } }
    abort:
        OLED_CS_Set();
}

/* DMA 异步状态机发送一页 */
static void OLED_SendPage_DMA(uint8_t page)
{
    OLED_SetCursor_ISR(page, 0);
    OLED_DC_Set();
    OLED_CS_Clr();
    if (HAL_SPI_Transmit_DMA(&hspi1, OLED_DisplayBuf[page], 128) != HAL_OK) {
        OLED_CS_Set(); // 发送启动失败时立即回滚状态，防止 CS 永久挂起低电平
        s_oled_dma_busy = false;
        s_oled_updating = false;
        s_oled_current_page = 0;
        return;
    }
    s_oled_dma_busy = true;
}

/* SPI DMA 发送完成中断回调：非阻塞流水线触发下一页 */
void OLED_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1)
    {
        OLED_CS_Set();
        s_oled_dma_busy = false;

        if (s_oled_updating)
        {
            s_oled_current_page++;
            if (s_oled_current_page < 16)
            {
                OLED_SendPage_DMA(s_oled_current_page);
            }
            else
            {
                s_oled_updating = false;
                s_oled_current_page = 0;
            }
        }
    }
}

/* 更新显存到OLED (纯异步非阻塞 + 脏标记检测) */
void OLED_Update(void)
{
    if (s_oled_dma_busy || s_oled_updating)
    {
        return; /* 若上一帧尚未发完则不打断，防止总线竞争 */
    }

    /* 对比脏标记：如果显存无变化，则直接跳过 DMA 刷屏，大幅节约 CPU 与总线 */
    if (memcmp(s_oled_last_frame, OLED_DisplayBuf, sizeof(OLED_DisplayBuf)) == 0)
    {
        return;
    }

    memcpy(s_oled_last_frame, OLED_DisplayBuf, sizeof(OLED_DisplayBuf));

    s_oled_updating = true;
    s_oled_current_page = 0;
    OLED_SendPage_DMA(0);
}

void OLED_Update_DisplayBuf(uint8_t DisplayBuf[16][128])
{
    if (s_oled_dma_busy || s_oled_updating) return; // 先判忙，DMA 传输中绝不 memcpy 显存
    memcpy(OLED_DisplayBuf, DisplayBuf, sizeof(OLED_DisplayBuf));
    UartComm_MirrorFrame(OLED_DisplayBuf);          // 脏帧 -> PC 镜像推流 (连接时, 零阻塞丢帧)
    OLED_Update();
}

void OLED_UpdateArea(uint8_t X, uint8_t Y, uint8_t Width, uint8_t Height)
{
    OLED_Update();
}

extern void OLED_Clear(void);

/* OLED的初始化 */
void OLED_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();

    OLED_RES_Clr();
    for (volatile uint32_t i = 0; i < 20000; i++);
    OLED_RES_Set();
    for (volatile uint32_t i = 0; i < 20000; i++);

    OLED_Write_CMD(0xAE); // --turn off oled panel
    OLED_Write_CMD(0xd5); // Set Frame Frequency
    OLED_Write_CMD(0xF0); // 调至最高刷新等级
    OLED_Write_CMD(0x20); // Set Memory Addressing Mode
    OLED_Write_CMD(0x81); // Set Contrast Control
    OLED_Write_CMD(0x4f);
    OLED_Write_CMD(0xad); // Set DC/DC off
    OLED_Write_CMD(0x8a);

    OLED_Write_CMD(0xC0);
    OLED_Write_CMD(0xA0);

    OLED_Write_CMD(0xdc); // Set Display Start Line
    OLED_Write_CMD(0x00);
    OLED_Write_CMD(0xd3); // Set Display Offset
    OLED_Write_CMD(0x00);
    OLED_Write_CMD(0xd9); // Set Discharge / Pre-Charge Period
    OLED_Write_CMD(0x22);
    OLED_Write_CMD(0xdb); // Set Vcomh voltage
    OLED_Write_CMD(0x35);

    OLED_Write_CMD(0xa8); // Set Multiplex Ratio
    OLED_Write_CMD(0x7f);

    OLED_Write_CMD(0xa4); // Set Entire Display OFF/ON
    OLED_Write_CMD(0xa6); // Set Normal/Reverse Display

    OLED_Clear();
    for (uint8_t j = 0; j < 16; j++)
    {
        OLED_SetCursor(j, 0);
        OLED_DC_Set();
        OLED_CS_Clr();
        HAL_SPI_Transmit(&hspi1, OLED_DisplayBuf[j], 128, 100);
        OLED_CS_Set();
    }
    OLED_Write_CMD(0xAF); // Display ON
}

/* OLED设置亮度 */
void OLED_Brightness(int16_t Brightness)
{
    if (Brightness > 255) Brightness = 255;
    if (Brightness < 0) Brightness = 0;
    OLED_Write_CMD(0x81);
    OLED_Write_CMD(Brightness);
}

/* 设置显示模式 */
void OLED_SetColorMode(bool colormode)
{
    OLED_ColorMode = colormode;
}
