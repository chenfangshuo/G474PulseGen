/*
 * Copyright (c) Sheep118 (WouoUI-PageVersion)
 *           https://github.com/Sheep118/WouoUI-PageVersion
 * Copyright (c) 2025 chenfangshuo (modifications)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#ifndef __WOUOUI_ANIM__
#define __WOUOUI_ANIM__

#ifdef __cplusplus
extern "C" {
#endif

#include "WouoUI_common.h"

typedef struct _AnimPos {
    int16_t pos_cur;
    int16_t pos_tgt;
    int16_t pos_err;
} AnimPos;

void WouoUI_Animation(AnimPos *animStr, uint16_t aniTime, uint16_t inrTime, uint8_t* ret);

#ifdef __cplusplus
}
#endif

#endif
