#include "WouoUI_anim.h"

/**
 * @brief 非线性运动函数
 *
 * @param animStr[in/out] 动画结构体
 * @param aniTime[in] 动画时间参数
 * @param inrtime[in] 轮序间隔时间
 * @param ret[out] 动画是否结束的结果指针(用于统计所有动画是否结束,true表示结束)
 */
void WouoUI_Animation(AnimPos *animStr, uint16_t aniTime, uint16_t inrTime, uint8_t *ret) {
    uint8_t temp = false;
    int16_t n = (inrTime == 0u) ? 1 : (int16_t)(aniTime / inrTime);
    if (n <= 0) n = 1;                                  /* 防 0 除 + 防状态机停滞 */
    if (animStr->pos_cur != animStr->pos_tgt) {
        animStr->pos_err += (animStr->pos_tgt - animStr->pos_cur);
        animStr->pos_cur += animStr->pos_err / n;
        animStr->pos_err %= n;
    } else { animStr->pos_err = 0; temp = true; }
    (*ret) = temp && (*ret);
}

