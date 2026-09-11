#include "Preset.h"
#include "main.h"
#include "Pulse.h"
#include "WouoUI.h"
#include "WouoUI_user.h"   /* UI 数据模型数组的唯一声明来源 */
#include <string.h>

/* 注: 原此处有 10 条 UI 数组的 extern (7 个 Option[] + 3 个 String[]),
 * 与 WouoUI_user.h 中的声明重复且无单一来源, 已删除。 */

/* ---------- Flash 布局 ----------
 * 参数存在 Flash 的**最后一页** (本板 512KB 型号 -> 2KB @ 0x0807F800)。
 * 代码区约 106KB, 距页首很远, 不冲突。
 *
 * 地址由「容量 − 页大小」推导, **不写死**:
 *   FLASH_SIZE      由 HAL 从芯片出厂容量寄存器 (FLASHSIZE_BASE) 运行时读出,
 *                   512KB 型号得到 0x80000, 256KB 得到 0x40000, 自动跟随;
 *   FLASH_PAGE_SIZE 取自 HAL (G4 全系 2KB)。
 * 因此换用 128KB / 256KB 的 G4 时地址自动正确, 无需改动本文件。
 * (原实现写死 0x0807F800 —— 在非 512KB 型号上会落到非法地址。)
 *
 * ⚠ 换芯片后仍须确认链接脚本 STM32G474XX_FLASH.ld 的 FLASH LENGTH 与之匹配,
 *   否则代码区可能与参数页重叠。完整移植清单见根目录 PORTING.md。 */
#define PRESET_FLASH_ADDR    (FLASH_BASE + FLASH_SIZE - FLASH_PAGE_SIZE)
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
/**
 * @brief  计算参数结构的校验和
 * @param  d  待计算的数据结构
 * @return 校验值 (调用方同时写入 d->crc, 供下次 Load 比对)
 *
 * @note   算法是**自研的简单求和 + 版本异或**, 并非标准 CRC:
 *           sum = Σ(w[3..n]) ^ version
 *         跳过前 3 个 32 位字 (magic / version / crc 自身), 避免自引用。
 *
 * @warning 这不是强校验, 只能发现多数位翻转, 不能抵御蓄意构造。
 *          它的目的是回答"Flash 读回的数据是否可信"。
 *          **不要**单方面替换成"更标准"的 CRC —— version 参与异或,
 *          换算法必须同步提升 PRESET_VERSION, 否则旧数据会被误判为有效。
 * @note   按 32 位字遍历要求 PresetData_t 内部**无填充洞**且 4 字节对齐。
 *          改动该结构体的成员类型/顺序时务必检查这一点, 否则会读到未初始化
 *          字节, 导致校验随机失败 (表现为"保存正常但下次开机读不回来")。
 */
static uint32_t Preset_CalcCrc(const PresetData_t *d)
{
    const uint32_t *w = (const uint32_t *)d;
    uint32_t sum = 0;
    /* 跳过 magic(w0) / version(w1) / crc(w2), 累加其余所有数据字 */
    for (uint32_t i = 3; i < sizeof(PresetData_t) / 4U; i++) sum += w[i];
    return sum ^ d->version;
}

/* ---------- Flash 底层 ---------- */
/**
 * @brief  擦除参数存储页 (Flash 最后一页, 本板 2KB @ 0x0807F800)
 * @retval true  擦除成功
 * @retval false HAL 返回非 HAL_OK
 *
 * @note   目标 bank 依 FLASH_OPTR 的 DBANK 选项位**运行时**判定:
 *           DBANK=1 (双 bank): bank2 —— 最后一页在该 bank 内
 *           DBANK=0 (单 bank): bank1
 *
 * @note   页号取 FLASH_PAGE_NB - 1, **不写死**。FLASH_PAGE_NB 由 HAL 依
 *          容量与 DBANK 自动分支 (双 bank 时是"每 bank 页数", 单 bank 时是
 *          "总页数"), 因此 512KB/256KB/128KB 都能得到正确的末页号。
 *          对应关系: 512KB 双 bank -> 127, 512KB 单 bank -> 255,
 *                    256KB 双 bank -> 63,  256KB 单 bank -> 127
 *          (原实现写死 127/255 —— 只对 512KB 成立, 换容量会擦到错误的页。)
 *
 * @warning 若改动存储位置, 必须**同时**修改 PRESET_FLASH_ADDR 与本函数,
 *          并确认所选页在链接脚本 STM32G474XX_FLASH.ld 的 FLASH 区域之外
 *          —— 否则会擦掉程序代码。移植时见根目录 PORTING.md。
 * @note   擦除前清全部 Flash 错误标志, 避免上一次操作遗留的 ECC 错误
 *          导致本次擦除直接失败。
 */
static bool Preset_ErasePage(void)
{
    FLASH_EraseInitTypeDef e = {0};
    uint32_t page, bank;

    /* 最后一页所在的 bank; 页号统一取该 bank 的末页 */
    if (READ_BIT(FLASH->OPTR, FLASH_OPTR_DBANK) != 0U)
    {
        bank = FLASH_BANK_2;   /* 双 bank: 最后一页在 bank2 */
    }
    else
    {
        bank = FLASH_BANK_1;   /* 单 bank */
    }
    page = FLASH_PAGE_NB - 1U;

    /* 合理性护栏: 推导出的地址必须页对齐且**确实落在 Flash 区内**, 否则拒绝擦除。
     *
     * 这道检查是随"地址由运行时求值"一起加的: PRESET_FLASH_ADDR 依赖 HAL 从
     * 芯片容量寄存器读出的 FLASH_SIZE, 而该值不再由编译器保证。假如它读到 0,
     * FLASH_PAGE_NB 会落到最后那个 else 分支 (32 或 64), 求出的页号将指向
     * **代码区** —— 擦掉即不可恢复。宁可存储功能失败, 也不能擦错页。
     * (正常芯片上此检查恒为真, 不产生额外开销。) */
    if ((PRESET_FLASH_ADDR & (FLASH_PAGE_SIZE - 1U)) != 0U ||
        (PRESET_FLASH_ADDR < FLASH_BASE) ||
        (PRESET_FLASH_ADDR >= (FLASH_BASE + FLASH_SIZE)))
    {
        return false;
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

/**
 * @brief  把参数结构逐双字写入 Flash
 * @param  d  待写入的数据
 * @retval true  全部双字写入成功
 * @retval false 某个双字写入失败 (已加锁后返回)
 *
 * @note   必须按**双字 (64bit)** 编程 —— 这是 STM32G4 Flash 的最小写入粒度。
 *         用 memcpy 逐 8 字节取数是为了规避未对齐访问: PresetData_t 内部
 *         未必 8 字节对齐, 直接按 uint64_t* 解引用会触发 HardFault。
 *
 * @warning 写入前该页必须已擦除 (Flash 只能 1→0 单向写入)。本函数**不负责
 *          擦除**, 由调用方 Preset_Save() 先调用 Preset_ErasePage()。
 * @note   所有失败路径都先 HAL_FLASH_Lock() 再返回, 不要漏掉加锁 ——
 *          未加锁会让后续任何代码都能误写 Flash。
 */
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
/**
 * @brief  保存当前全部 UI 参数到 Flash
 * @retval true  保存成功
 * @retval false 擦除或写入失败
 *
 * @note   数据来源是各模式的 `*_option_array[].val` / `.content` (UI 数据模型,
 *         定义于 WouoUI_user.c) —— 即**保存的是 UI 上显示的值**, 而不是从
 *         硬件寄存器回读的实际值。因此调用前应确保 UI 与硬件已同步。
 * @note   下拉选项 (通道/极性/互补对) 存的是**索引**而非字符串, 由
 *         Preset_ChannelIdx / Preset_PolarityIdx / Preset_PairIdx 做转换;
 *         转换失败返回 0 (即默认选项), 这是有意的容错。
 *
 * @warning 结构体的成员顺序/类型变更等同于 Flash 布局变更, **必须同步提升
 *          PRESET_VERSION** —— 否则新固件会把旧固件写的数据按错误布局解读,
 *          且因 magic 相同而不会拒绝。
 * @note   完整写入流程: 组装 -> 算 CRC -> 擦页 -> 逐双字编程。
 */
bool Preset_Save(void)
{
    PresetData_t d = {0};
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
    switch (g_pulse_mode)
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
