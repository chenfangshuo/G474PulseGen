/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    uart_comm.c
  * @brief   PC 通信协议层 (OLED 镜像推流 + SCPI 文本控制 + 虚拟按键注入)
  *
  *  帧格式: <AA 55 A5> <TYPE> <LEN:LE> <Payload> <CRC16:LE>
  *    - T=0x01 BTN  PC->MCU 虚拟按键       载荷=1..N 键码
  *    - T=0x02 CMD  PC->MCU SCPI ASCII 命令  载荷含 '\n' 结尾
  *    - T=0x03 PING 双向心跳
  *    - T=0x10 FRAME MCU->PC 屏幕镜像 2048B
  *    - T=0x11 RSP  MCU->PC SCPI 响应 ASCII
  *    - T=0x12 ACK  MCU->PC 握手/连接确认
  *  CRC16-CCITT (0x1021, init 0xFFFF), 覆盖 TYPE..Payload
  *
  * 连接状态机: 收到任一合法 PC 帧 -> LINKED; HAL_GetTick 2s 无数据 -> IDLE,
  *             IDLE 暂停 FRAME 镜像推流, 但 SCPI/虚拟按键始终有效。
  *
  * 发送: Ping-Pong 双缓冲 + HAL_UART_Transmit_DMA, 零 CPU 阻塞(忙则丢帧)。
  * @note  手写自维护文件, UartComm_Proc() 由 main 主循环 ~90Hz 调用
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "uart_comm.h"
#include "usart.h"
#include "Pulse.h"
#include "Preset.h"
#include "WouoUI.h"
#include "WouoUI_user.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* USER CODE BEGIN 0 */

/* ---------------------------- 模块变量 ----------------------------------- */

/* 连接状态机 */
static volatile UartComm_LinkState s_link_state = UC_IDLE;
static volatile uint32_t s_last_rx_tick = 0u;   /* 最近一次收到合法 PC 帧的时刻 */

/* RX 环形缓冲 (中断内仅入队, 主循环消费) */
#define UC_RX_RING_SIZE   512u
static volatile uint8_t  s_rx_ring[UC_RX_RING_SIZE];
static volatile uint16_t s_rx_head = 0u;        /* 写入索引 (ISR) */
static volatile uint16_t s_rx_tail = 0u;        /* 读取索引 (Proc) */

/* RX 帧解析状态机 */
typedef enum {
    RX_SYNC0, RX_SYNC1, RX_SYNC2,
    RX_TYPE, RX_LEN0, RX_LEN1,
    RX_PAYLOAD, RX_CRC0, RX_CRC1
} UcRxState;
static UcRxState s_rx_state = RX_SYNC0;
static uint8_t   s_rx_type = 0u;
static uint16_t  s_rx_len = 0u;
static uint16_t  s_rx_idx = 0u;
static uint16_t  s_rx_crc = 0xFFFFu;
static uint8_t   s_rx_payload[UC_CMD_MAX];     /* 接收方向载荷上限 (恰好容纳 CMD) */

/* TX Ping-Pong 双缓冲 */
#define UC_MAX_FRAME   UC_TOTAL_MAX             /* = 2056 字节整帧 */
#define UC_TX_NONE     2u                       /* bank 无在飞/无待发 */
/* TX 发送优先级: 越高越优先, 新帧优先级 > 待发帧才覆盖, 否则丢新帧。
 * 命令响应(CRIT)永不丢弃, 可覆盖 ACK/STAT 推送/镜像帧, 保证请求-响应对应 */
typedef enum {
    UC_TX_PRIO_FRAME = 0u,   /* 镜像帧: 忙则丢帧 */
    UC_TX_PRIO_SOFT  = 1u,   /* ACK/STAT 主动推送/首帧: 可被命令响应覆盖 */
    UC_TX_PRIO_CRIT  = 2u    /* 命令响应 RSP: 最高, 不可丢弃 */
} UcTxPrio;
static uint8_t  s_tx_frames[2][UC_MAX_FRAME] = {0u};
static volatile uint8_t  s_tx_active = UC_TX_NONE;  /* 当前在飞 bank 索引 */
static volatile uint8_t  s_tx_pending = UC_TX_NONE; /* 待发 bank 索引 (主循环写, ISR 读) */
static volatile uint8_t  s_tx_pending_prio = UC_TX_PRIO_FRAME;  /* 待发帧优先级 (主循环写, ISR 读) */
static volatile uint16_t s_tx_len[2] = {0u, 0u};    /* 每 bank 帧长 (ISR 读) */

/* RSP 响应缓冲 */
static char s_rsp_buf[UC_CMD_MAX];

/* RLE 压缩缓冲 + 最近帧缓存 (连接建立时主动推一帧, 避免 PC 端打开后长时间纯色) */
static uint8_t s_rle_buf[UC_FRAME_RLE_MAX];        /* RLE 编码输出缓冲 */
static uint8_t s_frame_cache[UC_FRAME_LEN];        /* 最近一帧原始点阵 (主动推流用) */
static volatile bool s_frame_cache_valid = false;  /* 缓存是否有有效帧 */
static volatile bool s_force_frame = false;        /* 连接刚建立: 强制推一帧当前画面 */

/* RXNE 风暴保护: ISR 内计数, 超阈值立即关闭 RXNE (防拖死主循环), 主循环超时重新使能 */
#define UC_RX_STORM_THRESH   500u    /* 500 字节即视为噪声风暴 (正常 PING 远低于此) */
#define UC_RX_STORM_OFF_MS   500u    /* 关闭 RXNE 500ms, 给主循环恢复时间 */

/* 镜像推流节流: RLE 压缩后帧远小于 2048B (OLED 点阵大量连续 0x00/0xFF),
 * 921600 下压缩帧可到 60fps; 节流 16ms 即 ~60fps 上限, 链路忙则丢帧自动降频 */
#define UC_FRAME_MIN_MS      8u     /* ~120fps 上限: 匹配 TIM6 ~90Hz 动画步进, 避免 16ms 节流与 11ms 步进错拍致 45fps */

static volatile uint32_t s_rx_storm_cnt = 0u;
static volatile uint32_t s_rx_storm_off_until = 0u;  /* 关闭 RXNE 截止 tick (0=未关闭) */

/* 诊断计数 (SCPI STAT 可查, 便于远程定位) */
static volatile uint32_t s_diag_frame_sent = 0u;   /* 成功入队的镜像帧数 */
static volatile uint32_t s_diag_storm_trips = 0u;  /* RXNE 风暴触发次数 */
static volatile uint32_t s_diag_ore_cnt = 0u;      /* ORE 超载事件次数 */
static volatile uint32_t s_diag_rx_bytes = 0u;     /* 累计收到字节数 */
static volatile uint32_t s_last_frame_tick = 0u;   /* 上一镜像帧入队时刻 */
static volatile uint32_t s_storm_win_reset_tick = 0u;  /* 风暴计数滚动窗口复位基准 */

/* 状态变化主动推送快照: 监测 OUT/12V/模式/通道, 变化即推 STAT (上位机 0 延迟同步) */
static volatile uint8_t s_last_stat_out  = 0xFFu;
static volatile uint8_t s_last_stat_12v  = 0xFFu;
static volatile uint8_t s_last_stat_mode = 0xFFu;
static volatile uint8_t s_last_stat_ch   = 0xFFu;

/* 连接后周期性推帧兜底: 首帧偶发被串口噪声丢失时, 后续周期推帧补上 */
#define UC_LINK_PUSH_MS     3000u   /* 连接后 3s 内兜底 */
#define UC_LINK_PUSH_PERIOD 500u    /* 每 500ms 补推一帧 */
static volatile uint32_t s_link_push_until = 0u;

/* 12V_OUT 手动开关状态 (定义于 main.c, 由 Setting 页与 SCPI 共同写) */
extern volatile bool g_12v_enable;
/* Setting 页选项数组 (同步 12V Output 复选框 val 用) */
extern Option setting_option_array[];
/* OLED 当前显存 (连接建立时直接推显存, 避免缓存被连接前清屏空白污染) */
extern uint8_t OLED_DisplayBuf[16][128];

/* ---------------------------- 内部函数 ----------------------------------- */

/* RLE 编码 (定义于文件后部, 此处前置声明供 UartComm_Proc 调用) */
static uint16_t UcRleEncode(const uint8_t *src, uint16_t srclen, uint8_t *dst, uint16_t dstmax);

/* CRC16-CCITT 单字节更新 */
static inline uint16_t UcCrcByte(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)b << 8;                     /* 一次 CRC16-CCITT 迭代 */
    for (uint8_t i = 0; i < 8u; i++)
    {
        if (crc & 0x8000u)
            crc = (uint16_t)((crc << 1u) ^ 0x1021u);
        else
            crc = (uint16_t)(crc << 1u);
    }
    return crc;
}

/* 构建一帧写入指定 bank, 返回总长 */
static uint16_t UcBuildFrame(uint8_t type, const uint8_t *payload, uint16_t plen,
                             uint8_t bank)
{
    uint8_t *p = s_tx_frames[bank];
    uint16_t crc = 0xFFFFu;
    uint16_t i;

    p[0] = UC_SYNC0; p[1] = UC_SYNC1; p[2] = UC_SYNC2;
    p[3] = type;
    p[4] = (uint8_t)(plen & 0xFFu);
    p[5] = (uint8_t)((plen >> 8) & 0xFFu);
    memcpy(&p[6], payload, plen);

    crc = UcCrcByte(crc, type);
    crc = UcCrcByte(crc, (uint8_t)(plen & 0xFFu));
    crc = UcCrcByte(crc, (uint8_t)((plen >> 8) & 0xFFu));
    for (i = 0; i < plen; i++)
        crc = UcCrcByte(crc, payload[i]);

    p[6u + plen] = (uint8_t)(crc & 0xFFu);
    p[7u + plen] = (uint8_t)((crc >> 8) & 0xFFu);
    return (uint16_t)(8u + plen);               /* 6 头 + 载荷 + 2 CRC */
}

/* DMA 空闲则发起待发帧 */
static void UcTxKick(void)
{
    if (s_tx_active != UC_TX_NONE)
        return;                                   /* 上一帧尚在飞, 等待完成回调 */
    if (s_tx_pending != UC_TX_NONE)
    {
        uint8_t bank = s_tx_pending;
        s_tx_pending = UC_TX_NONE;
        s_tx_pending_prio = UC_TX_PRIO_FRAME;     /* 待发帧已移交 DMA, 清空优先级 */
        if (HAL_UART_Transmit_DMA(&huart3, s_tx_frames[bank], s_tx_len[bank]) == HAL_OK)
            s_tx_active = bank;                   /* 启动成功, 标记在飞 */
    }
}

/* 入队一帧发送。
 * prio: UC_TX_PRIO_* 优先级。新帧优先级 > 待发帧才覆盖, 否则丢新帧。
 * 命令响应(CRIT)永不丢弃, 可覆盖 ACK/STAT 推送/镜像帧, 保证请求-响应对应。 */
static bool UcTxQueue(uint8_t type, const uint8_t *payload, uint16_t plen, uint8_t prio)
{
    uint8_t bank;

    if (s_tx_pending != UC_TX_NONE)
    {
        if (prio <= s_tx_pending_prio)
            return false;                          /* 待发帧优先级不低, 丢新帧 (保护待发命令响应) */
        bank = s_tx_pending;                       /* 新帧优先级更高, 覆盖待发帧 */
    }
    else
    {
        bank = (s_tx_active == UC_TX_NONE) ? 0u : (uint8_t)(1u - s_tx_active);
    }

    s_tx_pending_prio = prio;
    s_tx_len[bank] = UcBuildFrame(type, payload, plen, bank);
    s_tx_pending = bank;
    if (type == UC_TYPE_FRAME || type == UC_TYPE_FRAME_RLE)
        s_diag_frame_sent++;                       /* 成功入队的镜像帧计数 (含 RLE) */
    UcTxKick();
    return true;
}

/* 发送 RSP 文本 (命令响应, 最高优先级 CRIT, 不可丢弃) */
static void UcSendResp(const char *txt)
{
    uint16_t len = (uint16_t)strlen(txt);
    if (len > UC_CMD_MAX - 1u) len = UC_CMD_MAX - 1u;
    UcTxQueue(UC_TYPE_RSP, (const uint8_t *)txt, len, UC_TX_PRIO_CRIT);
}

/* 发送 RSP 文本 (软优先级 SOFT: 状态主动推送, 可被命令响应覆盖) */
static void UcSendRespSoft(const char *txt)
{
    uint16_t len = (uint16_t)strlen(txt);
    if (len > UC_CMD_MAX - 1u) len = UC_CMD_MAX - 1u;
    UcTxQueue(UC_TYPE_RSP, (const uint8_t *)txt, len, UC_TX_PRIO_SOFT);
}

/* 构建并发送 STAT 状态响应: 供 SCPI STAT 查询(critical=true)与本地状态变化主动推送(critical=false)共用 */
static void UcSendStat(bool critical)
{
    const char *mode_name = "?";
    switch (PULSE_MODE)
    {
        case PULSE_MODE_NONE:           mode_name = "NONE";       break;
        case PULSE_MODE_NPULSE:         mode_name = "NPULSE";     break;
        case PULSE_MODE_DPULSE:         mode_name = "DPULSE";     break;
        case PULSE_MODE_PWM:            mode_name = "PWM";        break;
        case PULSE_MODE_NPULSE_LONG:    mode_name = "NPULSELONG"; break;
        case PULSE_MODE_PWM_LONG:       mode_name = "PWMLONG";    break;
        case PULSE_MODE_COMP_PWM:       mode_name = "COMPPWM";    break;
        case PULSE_MODE_COMP_PWM_LONG:  mode_name = "COMPPWMLONG"; break;
        default: break;
    }
    snprintf(s_rsp_buf, sizeof(s_rsp_buf),
             "MODE=%s;OUT=%s;12V=%u;CH=%u;FR=%lu;ST=%lu;OR=%lu;RX=%lu;LNK=%d",
             mode_name,
             PULSE_OUT_ENABLED ? "ON" : "OFF",
             (unsigned)g_12v_enable,
             (unsigned)(g_pulse_ctrl.channel + 1u),
             (unsigned long)s_diag_frame_sent,   /* 已发送镜像帧数 */
             (unsigned long)s_diag_storm_trips,  /* RXNE 风暴触发次数 */
             (unsigned long)s_diag_ore_cnt,      /* ORE 超载次数 */
             (unsigned long)s_diag_rx_bytes,     /* 累计接收字节 */
             (int)s_link_state);                 /* 1=已连接 0=空闲 */
    if (critical)
        UcSendResp(s_rsp_buf);        /* 查询响应: 不可丢弃 */
    else
        UcSendRespSoft(s_rsp_buf);    /* 主动推送: 可被命令响应覆盖 */
}

/* 虚拟按键码 -> WouoUI InputMsg 注入 */
static void UcInjectKey(uint8_t key)
{
    if (p_cur_ui == NULL)
        return;

    /* 鼠标滚轮: 按当前页面类型智能分发, 模拟标准 GUI 滚轮直觉 (菜单与数值方向都符合习惯)
     *   - 滑动数值弹窗 (ValWin):  上滚=增大(msg_right), 下滚=减小(msg_left)
     *   - 微调数值弹窗 (SpinWin): 上滚=增大/选中位左移(msg_up), 下滚=减小/选中位右移(msg_down)
     *     ⚠ SpinWin 用 msg_up/down(滚轮)而非 msg_left/right(编码器): 两者值方向相反(up=增大/left=减小), 需各自定方向
     *   - 菜单/列表/其它:          上滚=上移(msg_up), 下滚=下移(msg_down)
     * 与板载编码器(msg_left/right)解耦, 不影响其物理旋转方向 */
    if (key == UC_KEY_WHEEL_UP || key == UC_KEY_WHEEL_DOWN)
    {
        PageType pt = WouoUI_CheckPageType(WouoUI_GetCurrentPage());
        InputMsg up_msg, down_msg;
        if (pt == type_spinwin) {
            up_msg = msg_up;    /* SpinWin: msg_up=增大/选中位左移 */
            down_msg = msg_down;
        } else if (pt == type_slidevalwin) {
            up_msg = msg_right; /* ValWin:  msg_right=增大 */
            down_msg = msg_left;
        } else {
            up_msg = msg_up;    /* 菜单/列表: msg_up=上移 */
            down_msg = msg_down;
        }
        WOUOUI_MSG_QUE_SEND((key == UC_KEY_WHEEL_UP) ? up_msg : down_msg);
        return;
    }

    switch (key)
    {
        case UC_KEY_UP:    WOUOUI_MSG_QUE_SEND(msg_up);    break;
        case UC_KEY_DOWN:  WOUOUI_MSG_QUE_SEND(msg_down);  break;
        case UC_KEY_LEFT:  WOUOUI_MSG_QUE_SEND(msg_left);  break;
        case UC_KEY_RIGHT: WOUOUI_MSG_QUE_SEND(msg_right); break;
        case UC_KEY_ENTER: WOUOUI_MSG_QUE_SEND(msg_click); break;
        case UC_KEY_BACK:  WOUOUI_MSG_QUE_SEND(msg_return); break;
        default: break;                                    /* HOME 等暂不使用 */
    }
}

/* 处理 PING: 回 ACK + 记录连接 */
static void UcHandlePing(void)
{
    uint8_t st = (uint8_t)s_link_state;
    UcTxQueue(UC_TYPE_ACK, &st, 1u, UC_TX_PRIO_SOFT);
}

/* 同步当前模式的 @ Enable Output 复选框 val 到 UI。
 * SCPI OUTP:ON/OFF 只改了 PULSE_OUT_ENABLED(驱动 HRTIM), 但屏幕勾选框由
 * option_array[x].val 决定, 若不在此同步, 上位机切输出时屏幕勾选框不跟随。 */
static void UcSyncUiEnableOutput(uint8_t enabled)
{
    switch (PULSE_MODE)
    {
        case PULSE_MODE_NPULSE:      n_pulse_option_array[7].val = enabled;      break;
        case PULSE_MODE_NPULSE_LONG: n_pulse_long_option_array[6].val = enabled; break;
        case PULSE_MODE_DPULSE:      double_pulse_option_array[6].val = enabled; break;
        case PULSE_MODE_PWM:         pwm_option_array[5].val = enabled;          break;
        case PULSE_MODE_PWM_LONG:    pwm_long_option_array[5].val = enabled;     break;
        case PULSE_MODE_COMP_PWM:    comp_pwm_option_array[6].val = enabled;     break;
        case PULSE_MODE_COMP_PWM_LONG: comp_pwm_long_option_array[5].val = enabled; break;
        default: break;
    }
}

/* ---- SCPI 解析与分发 ---- */

static void UcScpiExec(const char *line)
{
    char buf[UC_CMD_MAX];
    char *tok;
    char *sub;
    size_t i;
    size_t n = strlen(line);

    /* 复制并大写化, 去尾部 \r\n */
    if (n >= sizeof(buf)) n = sizeof(buf) - 1u;
    for (i = 0; i < n; i++)
    {
        char c = line[i];
        if (c == '\r' || c == '\n') break;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[i] = c;
    }
    buf[i] = '\0';

    tok = strtok(buf, ":");
    if (tok == NULL) { UcSendResp("ERR EMPTY"); return; }

    /* ---------- *IDN? (仪器标识, PyVISA 兼容) ---------- */
    if (strcmp(tok, "*IDN?") == 0) { UcSendResp("PulseGen,G474-PulseGen,0001,1.0"); return; }

    /* ---------- OUTP:ON / OUTP:OFF ---------- */
    if (strcmp(tok, "OUTP") == 0)
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        if (strcmp(sub, "ON") == 0)    { Pulse_Enable_Output();  UcSyncUiEnableOutput(1u); UcSendResp("OK"); }
        else if (strcmp(sub, "OFF") == 0) { Pulse_Disable_Output(); UcSyncUiEnableOutput(0u); UcSendResp("OK"); }
        else UcSendResp("ERR ARG");
        return;
    }
    /* ---------- MODE:<mode> ---------- */
    if (strcmp(tok, "MODE") == 0)
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        if      (strcmp(sub, "NPULSE") == 0)         UserUi_SwitchMode(PULSE_MODE_NPULSE);
        else if (strcmp(sub, "DPULSE") == 0)         UserUi_SwitchMode(PULSE_MODE_DPULSE);
        else if (strcmp(sub, "PWM") == 0)            UserUi_SwitchMode(PULSE_MODE_PWM);
        else if (strcmp(sub, "NPULSELONG") == 0)     UserUi_SwitchMode(PULSE_MODE_NPULSE_LONG);
        else if (strcmp(sub, "PWMLONG") == 0)        UserUi_SwitchMode(PULSE_MODE_PWM_LONG);
        else if (strcmp(sub, "COMPPWM") == 0)        UserUi_SwitchMode(PULSE_MODE_COMP_PWM);
        else if (strcmp(sub, "COMPPWMLONG") == 0)    UserUi_SwitchMode(PULSE_MODE_COMP_PWM_LONG);
        else { UcSendResp("ERR MODE"); return; }
        UcSendResp("OK");                            /* 页面跳转/状态文本/Preset 已由 UserUi_SwitchMode 完成 */
        return;
    }

    /* ---------- CHAN:<n> ---------- */
    if (strcmp(tok, "CHAN") == 0)
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        int ch = atoi(sub);
        if (ch < 1 || ch > 6) { UcSendResp("ERR CH"); return; }
        UserUi_SetChannel((uint8_t)ch);               /* 同步硬件 + 屏幕 content 显示 */
        UcSendResp("OK");
        return;
    }

    /* ---------- POL:0 / POL:1 ---------- */
    if (strcmp(tok, "POL") == 0)
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        int pol = atoi(sub);
        if (pol != 0 && pol != 1) { UcSendResp("ERR POL"); return; }
        UserUi_SetPolarity((uint8_t)pol);             /* 同步硬件 + 屏幕 content 显示 */
        UcSendResp("OK");
        return;
    }

    /* ---------- 参数写入 (与 UI 回调同一写路径) ---------- */
    if (strcmp(tok, "PULS") == 0)          /* N 脉冲: 有效值/比例不变, val 单位×100 */
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        const char *vstr = strtok(NULL, ":");
        if (vstr == NULL) { UcSendResp("ERR VAL"); return; }
        float v = (float)atof(vstr);
        if      (strcmp(sub, "WIDTH") == 0)  n_pulse_option_array[3].val = (int32_t)(v * 100.0f);
        else if (strcmp(sub, "COUNT") == 0)  n_pulse_option_array[4].val = (int32_t)v;
        else if (strcmp(sub, "INTV") == 0)   n_pulse_option_array[5].val = (int32_t)(v * 100.0f);
        else { UcSendResp("ERR SUB"); return; }
        Pulse_nPulse_SetPW((float)n_pulse_option_array[3].val / 100.0f,
                           (float)n_pulse_option_array[5].val / 100.0f,
                           (uint32_t)n_pulse_option_array[4].val);
        UcSendResp("OK");
        return;
    }
    if (strcmp(tok, "DPULS") == 0)         /* 双脉冲: 1nd PW / Interval / 2nd PW, 单位 uS (整数 1~200) */
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        const char *vstr = strtok(NULL, ":");
        if (vstr == NULL) { UcSendResp("ERR VAL"); return; }
        int v = atoi(vstr);
        if      (strcmp(sub, "PW1") == 0)  double_pulse_option_array[3].val = v;
        else if (strcmp(sub, "INTV") == 0) double_pulse_option_array[4].val = v;
        else if (strcmp(sub, "PW2") == 0)  double_pulse_option_array[5].val = v;
        else { UcSendResp("ERR SUB"); return; }
        Pulse_dPulse_SetPW(double_pulse_option_array[3].val,
                           double_pulse_option_array[4].val,
                           double_pulse_option_array[5].val);
        UcSendResp("OK");
        return;
    }
    if (strcmp(tok, "PWM") == 0)
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        const char *vstr = strtok(NULL, ":");
        if (vstr == NULL) { UcSendResp("ERR VAL"); return; }
        float v = (float)atof(vstr);
        if      (strcmp(sub, "PER") == 0)  pwm_option_array[3].val = (int32_t)(v /** 1.0f*/);
        else if (strcmp(sub, "DUTY") == 0) pwm_option_array[4].val = (int32_t)(v /** 1.0f*/);
        else { UcSendResp("ERR SUB"); return; }
        Pulse_PWM_SetPW((float)pwm_option_array[3].val, pwm_option_array[4].val);
        UcSendResp("OK");
        return;
    }
    if (strcmp(tok, "COMP") == 0)
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        const char *vstr = strtok(NULL, ":");
        if (vstr == NULL) { UcSendResp("ERR VAL"); return; }
        float v = (float)atof(vstr);
        if (PULSE_MODE != PULSE_MODE_COMP_PWM) { UcSendResp("ERR MODE"); return; }
        if      (strcmp(sub, "PER") == 0) comp_pwm_option_array[2].val = (int32_t)(v * 100.0f);
        else if (strcmp(sub, "DUTY") == 0) comp_pwm_option_array[3].val = (int32_t)v;
        else if (strcmp(sub, "DTR") == 0)  comp_pwm_option_array[4].val = (int32_t)v;
        else if (strcmp(sub, "DTF") == 0)  comp_pwm_option_array[5].val = (int32_t)v;
        else { UcSendResp("ERR SUB"); return; }
        Pulse_CompPWM_SetPW((float)comp_pwm_option_array[2].val / 100.0f,
                            comp_pwm_option_array[3].val,
                            (uint32_t)comp_pwm_option_array[4].val,
                            (uint32_t)comp_pwm_option_array[5].val);
        UcSendResp("OK");
        return;
    }
    if (strcmp(tok, "LPWM") == 0)
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        const char *vstr = strtok(NULL, ":");
        if (vstr == NULL) { UcSendResp("ERR VAL"); return; }
        float v = (float)atof(vstr);
        if      (strcmp(sub, "PER") == 0)  pwm_long_option_array[3].val = (int32_t)(v * 1000.0f);
        else if (strcmp(sub, "DUTY") == 0) pwm_long_option_array[4].val = (int32_t)(v * 100.0f);
        else { UcSendResp("ERR SUB"); return; }
        Pulse_lPWM_SetPW((float)pwm_long_option_array[3].val / 1000.0f,
                         (float)pwm_long_option_array[4].val / 100.0f);
        UcSendResp("OK");
        return;
    }
    if (strcmp(tok, "BURST") == 0)         /* BURST:<prf Hz> 两段式 (0=单次, 1~100000) */
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        uint32_t prf = (uint32_t)strtoul(sub, NULL, 10);
        n_pulse_option_array[6].val = (int32_t)prf;
        Pulse_BurstPRF_Set(prf);               /* 0=单次, 1~100000Hz */
        UcSendResp("OK");
        return;
    }

    /* ---------- 单发/使能 ---------- */
    if (strcmp(tok, "TRIG") == 0)  { Trigger_Pulse();               UcSendResp("OK"); return; }
    if (strcmp(tok, "12V") == 0)
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        if      (strcmp(sub, "ON") == 0)  { g_12v_enable = true;  setting_option_array[1].val = 1; UcSendResp("OK"); }
        else if (strcmp(sub, "OFF") == 0) { g_12v_enable = false; setting_option_array[1].val = 0; UcSendResp("OK"); }
        else UcSendResp("ERR ARG");
        return;
    }
    if (strcmp(tok, "PRESET") == 0)
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        if      (strcmp(sub, "SAVE") == 0) { UcSendResp(Preset_Save() ? "OK SAVED" : "ERR FLASH"); }
        else if (strcmp(sub, "LOAD") == 0) { UcSendResp(Preset_Load() ? "OK LOADED" : "ERR NOPRESET"); }
        else UcSendResp("ERR ARG");
        return;
    }

    /* ---------- HELP ---------- */
    if (strcmp(tok, "HELP") == 0)
    {
        /* 单条精简命令总览 (RSP 载荷上限 127B, 详细说明见上位机帮助面板/README) */
        UcSendResp("CMDS: OUTP:ON/OFF TRIG 12V:ON/OFF MODE CHAN POL PRESET:SAVE/LOAD STAT KEY:1-6 PULS DPULS PWM LPWM COMP BURST");
        return;
    }

    /* ---------- STAT? ---------- */
    if (strcmp(tok, "STAT") == 0) { UcSendStat(true); return; }

    /* ---------- KEY:<n> ---------- */
    if (strcmp(tok, "KEY") == 0)
    {
        sub = strtok(NULL, ":");
        if (sub == NULL) { UcSendResp("ERR ARG"); return; }
        UcInjectKey((uint8_t)atoi(sub));
        UcSendResp("OK");
        return;
    }

    UcSendResp("ERR CMD");
}

/* 处理一帧合法载荷 */
static void UcDispatch(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint16_t i;

    /* 任何合法 PC 帧刷新连接时间戳 (PC 每 500ms PING) */
    s_last_rx_tick = HAL_GetTick();
    if (s_link_state == UC_IDLE)
    {
        s_link_state = UC_LINKED;
        s_force_frame = true;    /* 连接刚建立: 立即推一帧当前画面, 消除 PC 端打开后纯色等待 */
        /* 连接建立即更新状态快照: 否则首帧与"状态变化推送的 STAT"在同一轮都触发,
         * 两者均为高优先级, 状态推送会覆盖 pending 中的首帧, 导致 PC 端收不到第一帧 */
        s_last_stat_out  = PULSE_OUT_ENABLED ? 1u : 0u;
        s_last_stat_12v  = g_12v_enable ? 1u : 0u;
        s_last_stat_mode = PULSE_MODE;
        s_last_stat_ch   = (uint8_t)g_pulse_ctrl.channel;
        s_link_push_until = HAL_GetTick() + UC_LINK_PUSH_MS;   /* 启动首帧兜底推帧窗口 */
    }

    switch (type)
    {
        case UC_TYPE_BTN:
            for (i = 0; i < len; i++)
                UcInjectKey(payload[i]);          /* 一帧可注入多键 */
            break;
        case UC_TYPE_CMD:
            if (len > 0u)
            {
                if (len < UC_CMD_MAX)
                    s_rx_payload[len] = '\0';   /* 补终止符: PC 发纯 ASCII 无 \0, 防 strlen 读残留 */
                UcScpiExec((const char *)payload);
            }
            break;
        case UC_TYPE_PING:
            UcHandlePing();
            break;
        default:
            break;
    }
}

/* ---------------------------- 对外接口 ----------------------------------- */

/* 初始化: 复位状态机与连接 */
void UartComm_Init(void)
{
    s_link_state = UC_IDLE;
    s_last_rx_tick = 0u;
    s_rx_head = 0u; s_rx_tail = 0u;
    s_rx_state = RX_SYNC0;
    s_tx_active = UC_TX_NONE;
    s_tx_pending = UC_TX_NONE;
    s_tx_pending_prio = UC_TX_PRIO_FRAME;
    s_rx_storm_cnt = 0u;
    s_rx_storm_off_until = 0u;
    s_frame_cache_valid = false;   /* 复位缓存: 连接前清屏空白不再视为有效帧 */

    /* 上电延迟 50ms: 等电源/模块 TX 电平稳定后再使能 RXNE, 防上电瞬间噪声 */
    HAL_Delay(50);

    /* 使能 UART 接收中断 (RXNE): USART3_IRQHandler 已在 it.c 定义 */
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_RXNE);
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_ORE);
}

/* USART3 ISR 调用: 逐字节入环形缓冲 (仅入队) + ISR 内风暴保护 */
void UartComm_RxByte(uint8_t byte)
{
    uint16_t next = (uint16_t)((s_rx_head + 1u) % UC_RX_RING_SIZE);
    s_diag_rx_bytes++;
    if (next != s_rx_tail)                        /* 满则丢弃 (PC 载荷远小于缓冲) */
    {
        s_rx_ring[s_rx_head] = byte;
        s_rx_head = next;
    }
    /* ISR 内风暴保护: 计数超阈值立即关闭 RXNE+ORE, 防止中断风暴拖死主循环 (屏幕卡住)
     * 必须同时关 ORE: RXNE 关闭后数据仍进 RDR, ORE 持续置位会继续触发 ISR */
    if (++s_rx_storm_cnt > UC_RX_STORM_THRESH)
    {
        s_diag_storm_trips++;
        __HAL_UART_DISABLE_IT(&huart3, UART_IT_RXNE);
        __HAL_UART_DISABLE_IT(&huart3, UART_IT_ORE);
        s_rx_storm_off_until = HAL_GetTick() + UC_RX_STORM_OFF_MS;
        s_rx_storm_cnt = 0u;
    }
}

/* USART3 ISR ORE 事件 (超载): 计数, 超阈值关闭 RXNE+ORE 防风暴 (与 RxByte 同保护) */
void UartComm_OreEvent(void)
{
    s_diag_ore_cnt++;
    if (++s_rx_storm_cnt > UC_RX_STORM_THRESH)
    {
        s_diag_storm_trips++;
        __HAL_UART_DISABLE_IT(&huart3, UART_IT_RXNE);
        __HAL_UART_DISABLE_IT(&huart3, UART_IT_ORE);
        s_rx_storm_off_until = HAL_GetTick() + UC_RX_STORM_OFF_MS;
        s_rx_storm_cnt = 0u;
    }
}

/* TX DMA 完成回调 (usart.c 转发): 释放在飞缓冲并继续发待发帧 */
void UartComm_TxComplete(void)
{
    if (s_tx_active != UC_TX_NONE)
        s_tx_active = UC_TX_NONE;
    UcTxKick();
}

/* 主循环 ~90Hz: 消费 ring 解析帧 + 心跳超时检查 + RXNE 风暴保护 */
void UartComm_Proc(void)
{
    uint8_t b;
    uint32_t now = HAL_GetTick();

    /* RXNE 风暴保护: ISR 已关闭 RXNE+ORE, 此处超时后重新使能 (风暴检测在 ISR 内) */
    if (s_rx_storm_off_until != 0u && now >= s_rx_storm_off_until)
    {
        __HAL_UART_ENABLE_IT(&huart3, UART_IT_RXNE);
        __HAL_UART_ENABLE_IT(&huart3, UART_IT_ORE);
        s_rx_storm_off_until = 0u;
    }

    /* 心跳超时: 2s 未收到 PC 帧 -> IDLE (暂停镜像推流) */
    if ((now - s_last_rx_tick) > UC_LINK_TIMEOUT_MS)
    {
        if (s_link_state != UC_IDLE)
            s_link_state = UC_IDLE;
    }

    /* 风暴计数滚动窗口: 每 100ms 清零, 阈值即"100ms 内 500 字节突发",
     * 而非单调累加, 避免长时间正常流量 (如 PING 18B/s) 累积误触发风暴保护 */
    if ((uint32_t)(now - s_storm_win_reset_tick) >= 100u)
    {
        s_storm_win_reset_tick = now;
        s_rx_storm_cnt = 0u;
    }

    /* 连接刚建立: 立即推一帧当前画面, 消除 PC 端打开后纯色等待 (需求 2)
     * 直接用当前显存 OLED_DisplayBuf, 不依赖缓存 —— 缓存可能在 TestUI_Init 清屏时
     * 被空白污染; 用高优先级推送, 确保首帧不被待发 RSP/ACK 覆盖丢弃 */
    if (s_force_frame)
    {
        s_force_frame = false;
        if (s_link_state == UC_LINKED)
        {
            uint16_t rlen = UcRleEncode((const uint8_t *)OLED_DisplayBuf, UC_FRAME_LEN, s_rle_buf, UC_FRAME_RLE_MAX);
            if (rlen > 0u)
                UcTxQueue(UC_TYPE_FRAME_RLE, s_rle_buf, rlen, UC_TX_PRIO_SOFT);
            else
                UcTxQueue(UC_TYPE_FRAME, (const uint8_t *)OLED_DisplayBuf, UC_FRAME_LEN, UC_TX_PRIO_SOFT);
            s_last_frame_tick = now;
        }
    }

    /* 连接后周期性推帧兜底: 首帧偶发丢失时, 每 500ms 补推一帧直到 3s 窗口结束。
     * 画面变化时的正常推流会刷新 s_last_frame_tick, 此处仅在静默时兜底 */
    if (s_link_state == UC_LINKED && now < s_link_push_until &&
        (uint32_t)(now - s_last_frame_tick) >= UC_LINK_PUSH_PERIOD)
    {
        uint16_t rlen = UcRleEncode((const uint8_t *)OLED_DisplayBuf, UC_FRAME_LEN, s_rle_buf, UC_FRAME_RLE_MAX);
        if (rlen > 0u)
            UcTxQueue(UC_TYPE_FRAME_RLE, s_rle_buf, rlen, UC_TX_PRIO_FRAME);
        else
            UcTxQueue(UC_TYPE_FRAME, (const uint8_t *)OLED_DisplayBuf, UC_FRAME_LEN, UC_TX_PRIO_FRAME);
        s_last_frame_tick = now;
    }

    /* 状态变化主动推送: 板上本地切换输出/12V/模式/通道后, 检测到变化立即推 STAT,
     * 上位机 0 延迟同步 (无需轮询)。快照在断开后复位为 0xFF 以触发下次重同步 */
    if (s_link_state == UC_LINKED)
    {
        uint8_t out  = PULSE_OUT_ENABLED ? 1u : 0u;
        uint8_t v12  = g_12v_enable ? 1u : 0u;
        uint8_t mode = PULSE_MODE;
        uint8_t ch   = (uint8_t)g_pulse_ctrl.channel;
        if (out != s_last_stat_out || v12 != s_last_stat_12v ||
            mode != s_last_stat_mode || ch != s_last_stat_ch)
        {
            s_last_stat_out = out;   s_last_stat_12v = v12;
            s_last_stat_mode = mode; s_last_stat_ch = ch;
            UcSendStat(false);
        }
    }
    else
    {
        s_last_stat_out = s_last_stat_12v = s_last_stat_mode = s_last_stat_ch = 0xFFu;
    }

    /* 消费 RX ring (每轮最多 64 字节, 防止垃圾数据持续填充时饿死 UI 刷新) */
    uint16_t processed = 0u;
    while (s_rx_tail != s_rx_head && processed < 64u)
    {
        processed++;
        b = s_rx_ring[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1u) % UC_RX_RING_SIZE);

        switch (s_rx_state)
        {
            case RX_SYNC0:
                if (b == UC_SYNC0) s_rx_state = RX_SYNC1;
                break;
            case RX_SYNC1:
                s_rx_state = (b == UC_SYNC1) ? RX_SYNC2 : RX_SYNC0;
                break;
            case RX_SYNC2:
                if (b == UC_SYNC2)
                {
                    s_rx_state = RX_TYPE;
                    s_rx_crc = 0xFFFFu;
                }
                else
                    s_rx_state = RX_SYNC0;
                break;
            case RX_TYPE:
                s_rx_type = b;
                s_rx_crc = UcCrcByte(s_rx_crc, b);
                s_rx_state = RX_LEN0;
                break;
            case RX_LEN0:
                s_rx_len = b;
                s_rx_crc = UcCrcByte(s_rx_crc, b);
                s_rx_state = RX_LEN1;
                break;
            case RX_LEN1:
                s_rx_len = (uint16_t)((uint16_t)b << 8 | s_rx_len);
                s_rx_crc = UcCrcByte(s_rx_crc, b);
                if (s_rx_len == 0u)
                    s_rx_state = RX_CRC0;         /* 空载荷直接进 CRC */
                else if (s_rx_len <= UC_CMD_MAX)
                {
                    s_rx_idx = 0u;
                    s_rx_state = RX_PAYLOAD;
                }
                else
                    s_rx_state = RX_SYNC0;        /* 载荷超上限: 丢弃 (约 2056) */
                break;
            case RX_PAYLOAD:
                s_rx_payload[s_rx_idx++] = b;
                s_rx_crc = UcCrcByte(s_rx_crc, b);
                if (s_rx_idx >= s_rx_len)
                    s_rx_state = RX_CRC0;
                break;
            case RX_CRC0:
                /* s_rx_crc 已是 TYPE..Payload 累加的计算 CRC, 与收到的 CRC0(低字节)直接比对 */
                if ((s_rx_crc & 0xFFu) == b)
                    s_rx_state = RX_CRC1;
                else
                    s_rx_state = RX_SYNC0;        /* CRC 低字节不匹配即丢弃 */
                break;
            case RX_CRC1:
                if ((uint8_t)((s_rx_crc >> 8) & 0xFFu) == b)
                {
                    /* 校验通过 */
                    s_rx_state = RX_SYNC0;
                    UcDispatch(s_rx_type, s_rx_payload, s_rx_len);
                }
                else
                    s_rx_state = RX_SYNC0;
                break;
            default:
                s_rx_state = RX_SYNC0;
                break;
        }
    }
}

/* OLED 点阵 RLE 编码: 返回压缩长度, 0 表示压缩后反而更大 (走原始帧)。
 * 编码规则: [count][byte], count=0 表示单字节字面量 (count=0 后跟 1 字面字节),
 * 否则 count 表示该字节重复次数 (1..255)。OLED 点阵大量连续 0x00/0xFF, 压缩比高。 */
static uint16_t UcRleEncode(const uint8_t *src, uint16_t srclen, uint8_t *dst, uint16_t dstmax)
{
    uint16_t i = 0, o = 0;
    while (i < srclen)
    {
        uint8_t b = src[i];
        uint16_t run = 1;
        while (i + run < srclen && src[i + run] == b && run < 255u)
            run++;
        if (run >= 3u)                      /* 游程 >=3 才压缩 (否则字面量更省) */
        {
            if (o + 2u > dstmax) return 0u;
            dst[o++] = (uint8_t)run;        /* count */
            dst[o++] = b;                   /* byte */
            i += run;
        }
        else
        {
            /* 字面量: count=0 后跟单字节 */
            if (o + 2u > dstmax) return 0u;
            dst[o++] = 0u;                  /* count=0 标记字面量 */
            dst[o++] = b;
            i += 1u;
        }
    }
    return o;
}

/* OLED_driver 在脏帧时调用: 镜像推流 (RLE 压缩, 零阻塞, 忙则丢帧) */
void UartComm_MirrorFrame(const uint8_t (*frame)[128])
{
    uint32_t now = HAL_GetTick();

    /* 缓存最近一帧 + 打标记 (未连接也缓存: 连接建立后由 Proc 主动推一次,
     * 消除 PC 端打开后纯色等待) */
    memcpy(s_frame_cache, (const uint8_t *)frame, UC_FRAME_LEN);
    s_frame_cache_valid = true;

    if (s_link_state != UC_LINKED)
        return;                                    /* 未连接: 暂停推流 (但已缓存最近帧) */

    /* 节流: 距上一帧 <16ms 跳过 (~60fps 上限, 链路忙则 UcTxQueue 丢帧自动降频) */
    if ((uint32_t)(now - s_last_frame_tick) < UC_FRAME_MIN_MS)
        return;
    s_last_frame_tick = now;

    /* RLE 压缩: 压缩后更小发 RLE, 否则回退原始帧 */
    uint16_t rlen = UcRleEncode((const uint8_t *)frame, UC_FRAME_LEN, s_rle_buf, UC_FRAME_RLE_MAX);
    if (rlen > 0u)
        UcTxQueue(UC_TYPE_FRAME_RLE, s_rle_buf, rlen, UC_TX_PRIO_FRAME);
    else
        UcTxQueue(UC_TYPE_FRAME, (const uint8_t *)frame, UC_FRAME_LEN, UC_TX_PRIO_FRAME);
}

UartComm_LinkState UartComm_GetLinkState(void)
{
    return s_link_state;
}

/* USER CODE END 0 */