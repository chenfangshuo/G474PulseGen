#ifndef __TEST_UI_H__
#define __TEST_UI_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "WouoUI.h"
void TestUI_Init(void);
void Pulse_Fault_HandleUI(void);   /* Fault 后 UI 收尾: 关 Enable 按钮 + 弹窗提示 */
extern WavePage wave_page;

/* ===== UI 数据模型:各模式 Option[] 数组 (SCPI 同步 UI 绑定变量用) ===== */
extern Option n_pulse_option_array[];
extern Option n_pulse_long_option_array[];
extern Option double_pulse_option_array[];
extern Option pwm_option_array[];
extern Option pwm_long_option_array[];
extern Option comp_pwm_option_array[];
extern Option comp_pwm_long_option_array[];

extern String ch_sel_str_array[];
extern String polarity_sel_str_array[];
#ifdef __cplusplus
}
#endif

#endif
