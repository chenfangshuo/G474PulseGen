#ifndef __PRESET_H
#define __PRESET_H

#include <stdbool.h>

/* Preset 参数存储/调出 (HAL_FLASH, 使用最后一页 2KB @ 0x0807F800) */
void Preset_ApplyMode(void);   /* 按当前模式从 UI 数组读取参数并施加到硬件 */
bool Preset_Save(void);        /* 保存当前全部参数到 Flash, 成功返回 true */
bool Preset_Load(void);        /* 从 Flash 调出参数写入 UI 数组, 无有效数据返回 false */

#endif /* __PRESET_H */
