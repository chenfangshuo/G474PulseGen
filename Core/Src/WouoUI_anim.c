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
        int16_t diff = (int16_t)(animStr->pos_tgt - animStr->pos_cur);
        animStr->pos_err += diff;
        int16_t step = animStr->pos_err / n;
        animStr->pos_err %= n;
        animStr->pos_cur += step;                 /* 先自然步进, step 随 diff 递减保留减速 */
        if (animStr->pos_cur == animStr->pos_tgt) {
            animStr->pos_err = 0;
            temp = true;                          /* 自然到达, 不打断减速 */
        } else if (step == 0 && diff >= -1 && diff <= 1) {
            /* 动量耗尽: 步进量已自然衰减为 0 且剩余 <=1px 才做终点吸附,
             * 消除 Bresenham 末尾停滞爬行, 同时保留减速缓冲 (不再步进前硬跳) */
            animStr->pos_cur = animStr->pos_tgt;
            animStr->pos_err = 0;
            temp = true;
        }
    } else { animStr->pos_err = 0; temp = true; }
    (*ret) = temp && (*ret);
}

