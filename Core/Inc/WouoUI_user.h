#ifndef WOUOUI_USER_H
#define WOUOUI_USER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "WouoUI.h"
void TestUI_Init(void);
void Pulse_Fault_HandleUI(void);   /* Fault 后 UI 收尾: 关 Enable 按钮 + 弹窗提示 */
/* SCPI 远程控制复用物理操作路径: 切模式/通道/极性 (含 UI 页面跳转与 content 同步) */
void UserUi_SwitchMode(uint8_t mode);
void UserUi_SetChannel(uint8_t ch);
void UserUi_SetPolarity(uint8_t pol);
/* 注: 原此处有 extern WavePage wave_page; 但全工程从未定义该对象、也无任何引用,
 * 系波形容器移除后遗留的悬空声明, 已删除。如需恢复波形页需同时补上定义。 */

/* ===== UI 数据模型 (定义均在 WouoUI_user.c) =====
 * 这些数组是 UI 与硬件参数之间的绑定变量, 被 SCPI (uart_comm.c) 与参数持久化
 * (Preset.c) 共享。**唯一声明来源就是本文件** —— 此前 main.c 与 Preset.c 各自
 * 手写了一份重复 extern, comp_pair_sel_str_array 与 setting_option_array 则
 * 完全没有声明来源, 现已统一收拢到此。
 * 注意: 它们不可加 static (跨文件使用)。 */
extern Option n_pulse_option_array[];
extern Option n_pulse_long_option_array[];
extern Option double_pulse_option_array[];
extern Option pwm_option_array[];
extern Option pwm_long_option_array[];
extern Option comp_pwm_option_array[];
extern Option comp_pwm_long_option_array[];

extern String ch_sel_str_array[];
extern String polarity_sel_str_array[];
extern String comp_pair_sel_str_array[];   /* 互补通道对下拉 (Preset.c 亦引用) */
extern Option setting_option_array[];      /* Setting 页选项 (uart_comm.c 同步 12V 复选框用) */
#ifdef __cplusplus
}
#endif

#endif
