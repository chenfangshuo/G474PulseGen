#include "Preset.h"
#include "main.h"
#include "Pulse.h"
#include "WouoUI.h"
#include <string.h>

/* ---------- 各模式 UI 参数数组 (定义于 WouoUI_user.c) ---------- */
extern Option n_pulse_option_array[];
extern Option n_pulse_long_option_array[];
extern Option double_pulse_option_array[];
extern Option pwm_option_array[];
extern Option pwm_long_option_array[];
extern Option comp_pwm_option_array[];
extern Option comp_pwm_long_option_array[];

extern String ch_sel_str_array[];
extern String polarity_sel_str_array[];
extern String comp_pair_sel_str_array[];

/* ---------- Flash 布局 ----------
   使用最后一页 (2KB @ 0x0807F800), 代码区约 106KB, 距离远不冲突 */
#define PRESET_FLASH_ADDR    0x0807F800UL
#define PRESET_MAGIC         0x50525354UL   /* "PRST" */
#define PRESET_VERSION       1U
#define PRESET_CH_NUM        6U
#define PRESET_POL_NUM       2U
#define PRESET_PAIR_NUM      3U

/* 存储结构: 全部用 int32, 通道/极性/互补对存数组索引 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc;
    /* Multi Pulse */
    int32_t np_ch, np_pol, np_width, np_count, np_interval, np_prf;
    /* Multi Pulse Long */
    int32_t npl_ch, npl_pol, npl_width, npl_count, npl_interval;
    /* Double Pulse */
    int32_t dp_ch, dp_pol, dp_pw1, dp_interval, dp_pw2;
    /* PWM */
    int32_t pwm_ch, pwm_pol, pwm_period, pwm_duty;
    /* PWM Long */
    int32_t pwml_ch, pwml_pol, pwml_period, pwml_duty;
    /* Comp PWM */
    int32_t cp_pair, cp_period, cp_duty, cp_dtr, cp_dtf;
    /* Comp PWM Long */
    int32_t cpl_pair, cpl_period, cpl_duty, cpl_dt;
} PresetData_t;

/* ---------- 内容字符串 -> 数组索引 ---------- */
static uint8_t Preset_ChannelIdx(String content)
{
    for (uint8_t i = 0; i < PRESET_CH_NUM; i++)
        if (content != NULL && strcmp(content, ch_sel_str_array[i]) == 0) return i;
    return 0;
}

static uint8_t Preset_PolarityIdx(String content)
{
    for (uint8_t i = 0; i < PRESET_POL_NUM; i++)
        if (content != NULL && strcmp(content, polarity_sel_str_array[i]) == 0) return i;
    return 0;
}

static uint8_t Preset_PairIdx(String content)
{
    for (uint8_t i = 0; i < PRESET_PAIR_NUM; i++)
        if (content != NULL && strcmp(content, comp_pair_sel_str_array[i]) == 0) return i;
    return 0;
}

/* ---------- CRC ---------- */
static uint32_t Preset_CalcCrc(const PresetData_t *d)
{
    const uint32_t *w = (const uint32_t *)d;
    uint32_t sum = 0;
    /* 跳过 magic(w0) / version(w1) / crc(w2), 累加其余所有数据字 */
    for (uint32_t i = 3; i < sizeof(PresetData_t) / 4U; i++) sum += w[i];
    return sum ^ d->version;
}

/* ---------- Flash 底层 ---------- */
static bool Preset_ErasePage(void)
{
    FLASH_EraseInitTypeDef e = {0};
    uint32_t page, bank;

    /* 依据 DBANK 选项位确定最后一页的 bank/page */
    if (READ_BIT(FLASH->OPTR, FLASH_OPTR_DBANK) != 0U)
    {
        bank = FLASH_BANK_2;   /* 双 bank: 最后一页在 bank2 */
        page = 127U;
    }
    else
    {
        bank = FLASH_BANK_1;   /* 单 bank: 最后一页 */
        page = 255U;
    }

    e.TypeErase = FLASH_TYPEERASE_PAGES;
    e.Banks     = bank;
    e.Page      = page;
    e.NbPages   = 1;

    uint32_t perr = 0;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&e, &perr);
    HAL_FLASH_Lock();
    return (st == HAL_OK);
}

static bool Preset_ProgramPage(const PresetData_t *d)
{
    const uint8_t *src = (const uint8_t *)d;
    uint32_t n = sizeof(PresetData_t) / 8U;   /* 双字(64bit)个数 */

    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < n; i++)
    {
        uint64_t word = 0;
        memcpy(&word, src + i * 8U, 8U);      /* 逐双字拷贝, 规避未对齐访问 */
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                              PRESET_FLASH_ADDR + i * 8U, word) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return false;
        }
    }
    HAL_FLASH_Lock();
    return true;
}

/* ---------- 保存: UI 数组 -> PresetData_t -> Flash ---------- */
bool Preset_Save(void)
{
    PresetData_t d;
    memset(&d, 0, sizeof(d));
    d.magic   = PRESET_MAGIC;
    d.version = PRESET_VERSION;

    /* Multi Pulse */
    d.np_ch       = Preset_ChannelIdx(n_pulse_option_array[1].content);
    d.np_pol      = Preset_PolarityIdx(n_pulse_option_array[2].content);
    d.np_width    = n_pulse_option_array[3].val;
    d.np_count    = n_pulse_option_array[4].val;
    d.np_interval = n_pulse_option_array[5].val;
    d.np_prf      = n_pulse_option_array[6].val;

    /* Multi Pulse Long */
    d.npl_ch       = Preset_ChannelIdx(n_pulse_long_option_array[1].content);
    d.npl_pol      = Preset_PolarityIdx(n_pulse_long_option_array[2].content);
    d.npl_width    = n_pulse_long_option_array[3].val;
    d.npl_count    = n_pulse_long_option_array[4].val;
    d.npl_interval = n_pulse_long_option_array[5].val;

    /* Double Pulse */
    d.dp_ch       = Preset_ChannelIdx(double_pulse_option_array[1].content);
    d.dp_pol      = Preset_PolarityIdx(double_pulse_option_array[2].content);
    d.dp_pw1      = double_pulse_option_array[3].val;
    d.dp_interval = double_pulse_option_array[4].val;
    d.dp_pw2      = double_pulse_option_array[5].val;

    /* PWM */
    d.pwm_ch     = Preset_ChannelIdx(pwm_option_array[1].content);
    d.pwm_pol    = Preset_PolarityIdx(pwm_option_array[2].content);
    d.pwm_period = pwm_option_array[3].val;
    d.pwm_duty   = pwm_option_array[4].val;

    /* PWM Long */
    d.pwml_ch     = Preset_ChannelIdx(pwm_long_option_array[1].content);
    d.pwml_pol    = Preset_PolarityIdx(pwm_long_option_array[2].content);
    d.pwml_period = pwm_long_option_array[3].val;
    d.pwml_duty   = pwm_long_option_array[4].val;

    /* Comp PWM */
    d.cp_pair   = Preset_PairIdx(comp_pwm_option_array[1].content);
    d.cp_period = comp_pwm_option_array[2].val;
    d.cp_duty   = comp_pwm_option_array[3].val;
    d.cp_dtr    = comp_pwm_option_array[4].val;
    d.cp_dtf    = comp_pwm_option_array[5].val;

    /* Comp PWM Long */
    d.cpl_pair   = Preset_PairIdx(comp_pwm_long_option_array[1].content);
    d.cpl_period = comp_pwm_long_option_array[2].val;
    d.cpl_duty   = comp_pwm_long_option_array[3].val;
    d.cpl_dt     = comp_pwm_long_option_array[4].val;

    d.crc = Preset_CalcCrc(&d);

    if (!Preset_ErasePage()) return false;
    return Preset_ProgramPage(&d);
}

/* ---------- 调出: Flash -> PresetData_t -> UI 数组 ---------- */
bool Preset_Load(void)
{
    const PresetData_t *src = (const PresetData_t *)PRESET_FLASH_ADDR;
    PresetData_t d;

    if (src->magic != PRESET_MAGIC || src->version != PRESET_VERSION) return false;

    memcpy(&d, src, sizeof(d));
    if (d.crc != Preset_CalcCrc(&d)) return false;

    /* 通道/极性/对索引钳制, 防止损坏数据越界 */
    d.np_ch    = (d.np_ch    >= 0 && d.np_ch    < (int32_t)PRESET_CH_NUM)  ? d.np_ch    : 0;
    d.np_pol   = (d.np_pol   >= 0 && d.np_pol   < (int32_t)PRESET_POL_NUM) ? d.np_pol   : 0;
    d.npl_ch   = (d.npl_ch   >= 0 && d.npl_ch   < (int32_t)PRESET_CH_NUM)  ? d.npl_ch   : 0;
    d.npl_pol  = (d.npl_pol  >= 0 && d.npl_pol  < (int32_t)PRESET_POL_NUM) ? d.npl_pol  : 0;
    d.dp_ch    = (d.dp_ch    >= 0 && d.dp_ch    < (int32_t)PRESET_CH_NUM)  ? d.dp_ch    : 0;
    d.dp_pol   = (d.dp_pol   >= 0 && d.dp_pol   < (int32_t)PRESET_POL_NUM) ? d.dp_pol   : 0;
    d.pwm_ch   = (d.pwm_ch   >= 0 && d.pwm_ch   < (int32_t)PRESET_CH_NUM)  ? d.pwm_ch   : 0;
    d.pwm_pol  = (d.pwm_pol  >= 0 && d.pwm_pol  < (int32_t)PRESET_POL_NUM) ? d.pwm_pol  : 0;
    d.pwml_ch  = (d.pwml_ch  >= 0 && d.pwml_ch  < (int32_t)PRESET_CH_NUM)  ? d.pwml_ch  : 0;
    d.pwml_pol = (d.pwml_pol >= 0 && d.pwml_pol < (int32_t)PRESET_POL_NUM) ? d.pwml_pol : 0;
    d.cp_pair  = (d.cp_pair  >= 0 && d.cp_pair  < (int32_t)PRESET_PAIR_NUM) ? d.cp_pair : 0;
    d.cpl_pair = (d.cpl_pair >= 0 && d.cpl_pair < (int32_t)PRESET_PAIR_NUM) ? d.cpl_pair : 0;

    /* Multi Pulse */
    n_pulse_option_array[1].content = ch_sel_str_array[d.np_ch];
    n_pulse_option_array[2].content = polarity_sel_str_array[d.np_pol];
    n_pulse_option_array[3].val = d.np_width;
    n_pulse_option_array[4].val = d.np_count;
    n_pulse_option_array[5].val = d.np_interval;
    n_pulse_option_array[6].val = d.np_prf;

    /* Multi Pulse Long */
    n_pulse_long_option_array[1].content = ch_sel_str_array[d.npl_ch];
    n_pulse_long_option_array[2].content = polarity_sel_str_array[d.npl_pol];
    n_pulse_long_option_array[3].val = d.npl_width;
    n_pulse_long_option_array[4].val = d.npl_count;
    n_pulse_long_option_array[5].val = d.npl_interval;

    /* Double Pulse */
    double_pulse_option_array[1].content = ch_sel_str_array[d.dp_ch];
    double_pulse_option_array[2].content = polarity_sel_str_array[d.dp_pol];
    double_pulse_option_array[3].val = d.dp_pw1;
    double_pulse_option_array[4].val = d.dp_interval;
    double_pulse_option_array[5].val = d.dp_pw2;

    /* PWM */
    pwm_option_array[1].content = ch_sel_str_array[d.pwm_ch];
    pwm_option_array[2].content = polarity_sel_str_array[d.pwm_pol];
    pwm_option_array[3].val = d.pwm_period;
    pwm_option_array[4].val = d.pwm_duty;

    /* PWM Long */
    pwm_long_option_array[1].content = ch_sel_str_array[d.pwml_ch];
    pwm_long_option_array[2].content = polarity_sel_str_array[d.pwml_pol];
    pwm_long_option_array[3].val = d.pwml_period;
    pwm_long_option_array[4].val = d.pwml_duty;

    /* Comp PWM */
    comp_pwm_option_array[1].content = comp_pair_sel_str_array[d.cp_pair];
    comp_pwm_option_array[2].val = d.cp_period;
    comp_pwm_option_array[3].val = d.cp_duty;
    comp_pwm_option_array[4].val = d.cp_dtr;
    comp_pwm_option_array[5].val = d.cp_dtf;

    /* Comp PWM Long */
    comp_pwm_long_option_array[1].content = comp_pair_sel_str_array[d.cpl_pair];
    comp_pwm_long_option_array[2].val = d.cpl_period;
    comp_pwm_long_option_array[3].val = d.cpl_duty;
    comp_pwm_long_option_array[4].val = d.cpl_dt;

    return true;
}

/* ---------- 按当前模式从 UI 数组施加参数到硬件 ---------- */
void Preset_ApplyMode(void)
{
    switch (PULSE_MODE)
    {
        case PULSE_MODE_NPULSE:
        {
            uint8_t ch = (uint8_t)Preset_ChannelIdx(n_pulse_option_array[1].content);
            Pulse_Select_Output((uint8_t)(ch + 1));
            if (n_pulse_option_array[2].content == polarity_sel_str_array[1])
                Pulse_SetPulsePolarity_Low();
            Pulse_nPulse_SetPW((float)n_pulse_option_array[3].val / 100.0f,
                               (float)n_pulse_option_array[5].val / 100.0f,
                               (uint32_t)n_pulse_option_array[4].val);
            Pulse_BurstPRF_Set((uint32_t)n_pulse_option_array[6].val);
            break;
        }
        case PULSE_MODE_NPULSE_LONG:
        {
            uint8_t ch = (uint8_t)Preset_ChannelIdx(n_pulse_long_option_array[1].content);
            Pulse_Select_Output((uint8_t)(ch + 1));
            if (n_pulse_long_option_array[2].content == polarity_sel_str_array[1])
                Pulse_SetPulsePolarity_Low();
            Pulse_nPulseLong_SetPW((float)n_pulse_long_option_array[3].val / 1000.0f,
                                   (float)n_pulse_long_option_array[5].val / 1000.0f);
            break;
        }
        case PULSE_MODE_DPULSE:
        {
            uint8_t ch = (uint8_t)Preset_ChannelIdx(double_pulse_option_array[1].content);
            Pulse_Select_Output((uint8_t)(ch + 1));
            if (double_pulse_option_array[2].content == polarity_sel_str_array[1])
                Pulse_SetPulsePolarity_Low();
            Pulse_dPulse_SetPW(double_pulse_option_array[3].val,
                               double_pulse_option_array[4].val,
                               double_pulse_option_array[5].val);
            break;
        }
        case PULSE_MODE_PWM:
        {
            uint8_t ch = (uint8_t)Preset_ChannelIdx(pwm_option_array[1].content);
            Pulse_Select_Output((uint8_t)(ch + 1));
            if (pwm_option_array[2].content == polarity_sel_str_array[1])
                Pulse_SetPulsePolarity_Low();
            Pulse_PWM_SetPW((float)pwm_option_array[3].val, pwm_option_array[4].val);
            break;
        }
        case PULSE_MODE_PWM_LONG:
        {
            uint8_t ch = (uint8_t)Preset_ChannelIdx(pwm_long_option_array[1].content);
            Pulse_Select_Output((uint8_t)(ch + 1));
            if (pwm_long_option_array[2].content == polarity_sel_str_array[1])
                Pulse_SetPulsePolarity_Low();
            Pulse_lPWM_SetPW((float)pwm_long_option_array[3].val / 1000.0f,
                             (float)pwm_long_option_array[4].val / 100.0f);
            break;
        }
        case PULSE_MODE_COMP_PWM:
        {
            uint8_t pair = (uint8_t)Preset_PairIdx(comp_pwm_option_array[1].content);
            Pulse_Select_CompPair(pair);
            Pulse_CompPWM_SetPW((float)comp_pwm_option_array[2].val / 100.0f,
                                (int32_t)comp_pwm_option_array[3].val,
                                (uint32_t)comp_pwm_option_array[4].val,
                                (uint32_t)comp_pwm_option_array[5].val);
            break;
        }
        case PULSE_MODE_COMP_PWM_LONG:
        {
            uint8_t pair = (uint8_t)Preset_PairIdx(comp_pwm_long_option_array[1].content);
            Pulse_Select_CompPair(pair);
            Pulse_CompLPWM_SetPW((float)comp_pwm_long_option_array[2].val / 1000.0f,
                                 (float)comp_pwm_long_option_array[3].val / 100.0f,
                                 (uint32_t)comp_pwm_long_option_array[4].val);
            break;
        }
        default:
            break;
    }
}
