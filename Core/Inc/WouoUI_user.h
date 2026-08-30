#ifndef __TEST_UI_H__
#define __TEST_UI_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "WouoUI.h"
void TestUI_Init(void);
void Pulse_Fault_HandleUI(void);   /* Fault 后 UI 收尾: 关 Enable 按钮 + 弹窗提示 */
extern WavePage wave_page;
#ifdef __cplusplus
}
#endif

#endif
