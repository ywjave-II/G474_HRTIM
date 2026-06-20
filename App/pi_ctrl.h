#ifndef PI_CTRL_H
#define PI_CTRL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ----------------------------------------------------------*/
#include "main.h"
#include "adc_app.h"    /* VOUT_MV_TO_CODE / g_vout_filt / ADC_VOUT_DIV_NUM/DEN */
#include "freq_skip.h"  /* llc_period */

/* ============================================================================
 *  PI 控制器 — 闭环调节 VOUT（PFM 调压，控制对象 = HRTIM 周期）
 *  --------------------------------------------------------------------------
 *  形式：增量式 PI（velocity form），定点 Q8.8 整数
 *
 *    delta_p = ((error - prev_error) * KP_INT) >> SHIFT    （增量 P：响应误差变化）
 *    delta_i = (error * KI_INT) >> SHIFT                    （增量 I：正比于当前误差）
 *    delta_u = delta_p + delta_i
 *    period  += delta_u      （经 slew rate + period 硬限幅）
 *
 *  增益方向：VOUT 偏低 → error 为正 → period 增大 → 降频增增益 → VOUT 回升
 *
 *  保护层级（由快到慢）：
 *    1. Slew rate 限制 — 单次 delta_u 钳位到 ±PI_PERIOD_SLEW_MAX
 *    2. Period 硬限幅  — 最终 period 钳位到 [PERIOD_MIN, PERIOD_MAX]
 *    3. Anti-Windup    — 饱和时冻结积分累加器 + 禁 delta_i
 *    4. 积分限幅       — integral 累加器钳位到 ±INTEGRAL_MAX/MIN << SHIFT
 *
 *  OVP 过压保护已移至 TIM3 ISR（adc_app.c），与 VOUT 采样同节拍，覆盖全状态。
 * ==========================================================================*/

/* ---- PI 增益（定点 Q8.8：实际值 = 宏 / 256）--------------------------- */
#define PI_KP_INT        256      /* Kp = 256/256 = 1.00  (tick / ADC码) */
#define PI_KI_INT        512      /* Ki = 512/256 = 2.00  — 无死区(error≥1码就有输出) */
#define PI_SHIFT         8U      /* Kp 和 Ki 共享 Q8.8 移位 */

/* ---- 目标电压 / VOUT 过压保护 ----------------------------------------------- */
#define PI_VOUT_TARGET_MV    24000U   /* 目标输出电压 (mV)，用户按需修改 */
#define PI_VOUT_OVP_MV       27000U   /* VOUT 过压保护阈值 (mV)：超过即封波进 FAULT */

/* ---- 周期限幅 ------------------------------------------------------------- */
#define PI_PERIOD_MIN    18133U      /* ≈300kHz，与软启动起点一致（最高频 = 最低增益）*/
#define PI_PERIOD_MAX    50000U      /* ≈108kHz，略低于谐振留余量 */

/* ---- 积分抗饱和（tick 量纲，与 period 同单位）--------------------------- */
#define PI_INTEGRAL_MAX_TICK   2000
#define PI_INTEGRAL_MIN_TICK  (-2000)

/* ---- 单次周期变化斜率限制（tick/次）— 防突变 --------------------------- */
#define PI_PERIOD_SLEW_MAX  1000U    /* 每次 PI 更新最多改变 ±100 tick */

/* Exported types -------------------------------------------------------- */
typedef struct {
    int32_t  integral;      /* 积分累加器（Q8.8 定点，诊断 + 限幅用）*/
    int32_t  error;         /* 最近一次误差（ADC 码），供串口诊断 */
    int32_t  prev_error;    /* 上一次误差（ADC 码），用于增量式 P */
    int32_t  p_term;        /* 最近 P 增量 delta_p（tick），供串口诊断 */
    int32_t  i_term;        /* 最近 I 增量 delta_i（tick），供串口诊断 */
    int32_t  delta_u;       /* 最近总增量（tick），供串口诊断 */
} pi_ctrl_t;

extern volatile pi_ctrl_t g_pi;

/* Exported functions ---------------------------------------------------- */

/* 上电/进入 RUN 时调用一次：积分清零、prev_error 清零、上次更新时刻清零 */
void PI_CTRL_Init(void);

/* 主循环每次迭代调用（内部用 HAL_GetTick 限速到 PI_UPDATE_MS）*/
void PI_CTRL_Step(void);

#ifdef __cplusplus
}
#endif

#endif /* PI_CTRL_H */
