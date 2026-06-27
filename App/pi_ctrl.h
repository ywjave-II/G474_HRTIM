#ifndef PI_CTRL_H
#define PI_CTRL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ----------------------------------------------------------*/
#include "main.h"
#include "freq_skip.h"  /* llc_period, HRTIM_SetLLCPeriod() */

/* ============================================================================
 *  PI 控制器 — 闭环调节 VOUT（PFM 调压，控制对象 = HRTIM 周期）
 *  --------------------------------------------------------------------------
 *  形式：增量式 PI（velocity form），浮点实现（利用 M4F 硬件 FPU）
 *
 *    delta_p = (error - prev_error) * Kp          （增量 P：响应误差变化率）
 *    delta_i = error * Ki                          （增量 I：正比于当前误差）
 *    delta_u = delta_p + delta_i
 *    period  += delta_u      （经 slew rate + period 硬限幅）
 *
 *  增益方向：VOUT 偏低 → error 为正 → period 增大 → 降频增增益 → VOUT 回升
 *
 *  保护层级（由快到慢）：
 *    1. OVP 过压      — 每次 10kHz 都检查，VOUT > 28V 立即关输出 + 置标志位
 *    2. Slew rate 限制 — 单次 delta_u 钳位到 ±PI_SLEW_MAX
 *    3. Period 硬限幅  — 最终 period 钳位到 [PERIOD_MIN, PERIOD_MAX]
 *    4. Anti-Windup    — 饱和时 delta_i=0，阻止 period 被继续推向饱和方向
 *    5. 死区           — |error| < DEADBAND_MV 时不更新 period，但持续更新 prev_error
 *
 *  分段 Kp（带滞回，防边界 chattering）：
 *    - 高频段 (period < SEG1)：Kp 较大，LLC 增益平坦区，需要更积极的调节
 *    - 中频段 (SEG1~SEG2)：Kp 适中，谐振点附近，增益变化剧烈
 *    - 低频段 (period > SEG2)：Kp 较小，接近谐振点，避免过调
 *
 *  执行时机：TIM3 10kHz ISR → PI_CTRL_Step()
 *    - 每次 10kHz 都执行 OVP 检查（Step 0）
 *    - 每 10 次执行一次 PI 计算（1kHz，Step 1~15）
 *
 *  关键参数说明（物理单位）：
 *    Kp 单位：tick/mV  — 每 mV 误差变化产生的周期修正量
 *    Ki 单位：tick/(mV·ms) — 每 mV 误差每 ms 产生的周期修正量（控制周期固定 1ms）
 *    error 单位：mV    — 目标电压 - 实测电压
 *    delta_u 单位：tick — 每次 PI 更新的周期增量
 * ==========================================================================*/

/* ============================================================================
 *  1. ADC → mV 换算参数（须与硬件分压电阻匹配）
 * ==========================================================================*/
/* VOUT 分压比：R_upper / R_lower + 1，例 10k/1k → 11:1 */
#define PI_VOUT_DIV_NUM     11.0f
#define PI_VOUT_DIV_DEN     1.0f

/* ADC 基准参数 */
#define PI_ADC_VREF_MV      3300.0f   /* ADC 参考电压 (mV) */
#define PI_ADC_FULLSCALE    4095.0f   /* 12bit 满量程 */

/* VOUT_SCALE：ADC 原始码 → 真实 VOUT 电压 (mV)
 *   = (VREF / FULLSCALE) * (DIV_NUM / DIV_DEN)
 *   = (3300 / 4095) * (11 / 1) ≈ 8.864 mV/码  */
#define PI_VOUT_SCALE  (((PI_ADC_VREF_MV) / (PI_ADC_FULLSCALE)) * \
                         ((PI_VOUT_DIV_NUM) / (PI_VOUT_DIV_DEN)))

/* ============================================================================
 *  2. 目标电压 / 过压保护（单位：mV）
 * ==========================================================================*/
#define PI_VOUT_TARGET_MV   24000.0f  /* 目标输出电压 (mV)，待实测校准 */
#define PI_VOUT_OVP_MV      28000.0f  /* VOUT 过压保护阈值 (mV)：超过即封波 */

/* ============================================================================
 *  3. 周期限幅（单位：tick）
 *      频率换算：f_sw = 5440e6 / period
 *      PERIOD_MIN = 18133 → ~300kHz（最高频 = 最低增益）
 *      PERIOD_MAX = 47000 → ~115.7kHz（fr 之下 M>1 区间，距容性区 ~6kHz 安全余量）
 * ==========================================================================*/
#define PI_PERIOD_MIN       18133.0f
#define PI_PERIOD_MAX       47000.0f

/* ============================================================================
 *  4. 分段 Kp 增益（单位：tick/mV）+ 分段边界（单位：tick）+ 滞回带（单位：tick）
 *     Kp 整定说明：
 *       - 高频段 LLC 增益曲线平坦 → Kp 可较大，快速响应
 *       - 接近谐振点时增益曲线变陡 → Kp 减小，避免过调/振荡
 *       - 滞回带 ±200 tick 防止 period 在边界附近来回穿越时 Kp 频繁切换
 * ==========================================================================*/
#define PI_KP_HIGH          0.5f     /* period < SEG1，高频段：响应快 */
#define PI_KP_MID           0.3f     /* SEG1~SEG2，中频段；从 1.0 降到 0.3 减少噪声放大 */
#define PI_KP_LOW           0.15f    /* period > SEG2，低频段（近谐振）：保守 */

#define PI_PERIOD_SEG1      24000.0f /* ~227kHz，高/中频分界 */
#define PI_PERIOD_SEG2      36000.0f /* ~151kHz，中/低频分界 */
#define PI_SEG_HYST         200.0f   /* 分段滞回带宽度 (tick) */

/* ============================================================================
 *  5. 积分增益（单位：tick/(mV·ms)）+ 死区 + 限速
 *     Ki 整定说明：
 *       - 控制周期固定 1ms，Ki 直接按此折算，无需 dt 补偿
 *       - Ki 必须足够大以消除稳态误差，但太大导致超调/振荡
 *       - 死区 ±30mV：噪声/纹波范围内不调节，避免 period 无意义微调
 * ==========================================================================*/
#define PI_KI               0.05f    /* 积分增益 (tick/(mV·ms))，待整定 */
#define PI_DEADBAND_MV      30.0f    /* 死区 ±30mV：误差在此范围内不调节 */
#define PI_SLEW_MAX         300.0f   /* 单次 delta_u 上限 (tick/次) */
#define PI_DECIMATION       10U     /* 10kHz→1kHz 分频比：每 10 次 ISR 执行 1 次 PI */

/* ============================================================================
 *  6. EWMA 低通滤波系数（α 越小越平滑，响应越慢）
 *     τ ≈ Ts/α = 1ms/0.03 ≈ 33ms @ 1kHz PI rate
 * ==========================================================================*/
#define PI_EWMA_ALPHA       0.03f    /* 滤波系数；原 0.1 太弱导致噪声经 P 项放大 */

/* ============================================================================
 *  7. 诊断结构体（全部使用 float，单位与计算中一致：mV、tick）
 * ==========================================================================*/
typedef enum {
    PI_SEG_HIGH = 0,   /* period < SEG1，高频段 */
    PI_SEG_MID  = 1,   /* SEG1 ~ SEG2，中频段 */
    PI_SEG_LOW  = 2    /* period > SEG2，低频段 */
} pi_segment_t;

typedef struct {
    /* ---- 实时控制状态（ISR 中更新）---- */
    float    vout_filt;         /* EWMA 滤波后的 VOUT (mV) */
    float    error;             /* 当前误差 (mV)：target - vout_filt */
    float    error_max;         /* 历史最大绝对误差 (mV)，Init 时清零 */
    float    prev_error;        /* 上一拍误差 (mV)，用于增量式 P */
    float    delta_p;           /* 最近 P 增量 (tick) */
    float    delta_i;           /* 最近 I 增量 (tick) */
    float    delta_u;           /* 最近总增量 (tick)：delta_p + delta_i */
    float    integral;          /* 积分累加器 (tick)，仅诊断用 */
    uint16_t vout_raw;          /* 最近 ADC 原始值 (0..4095) */
    uint8_t  kp_segment;        /* 当前 Kp 分段 (pi_segment_t) */

    /* ---- 累计统计（跨 RUN 累计，Init 时不清零）---- */
    uint32_t slew_clip_count;   /* slew rate 限幅累计次数 */
    uint32_t windup_count;      /* anti-windup 激活累计次数 */
    uint32_t ovp_count;         /* OVP 触发累计次数 */
} pi_ctrl_t;

/* 全局 PI 状态（ISR 写，主循环读，volatile 保证可见性）*/
extern volatile pi_ctrl_t g_pi;

/* ============================================================================
 *  8. 跨模块共享标志
 * ==========================================================================*/
/* ISR 中 OVP 触发时置 1，主循环 SafeSM_Poll 检测后调用 SafeSM_EnterFault 并清零。
 * 注：ISR 中已直接停 HRTIM 输出（硬件快路径），此标志仅用于状态机同步。*/
extern volatile uint8_t g_fault_request;

/* ============================================================================
 *  API
 * ==========================================================================*/

/* 进入 RUN 状态时调用一次：
 * - vout_filt 预置首样（避免 EWMA 从 0 爬升）
 * - 积分/prev_error 清零
 * - error_max 清零
 * - kp_segment 重置到 MID
 * （slew_clip_count / windup_count / ovp_count 跨 RUN 保留，不在此清零）*/
void PI_CTRL_Init(void);

/* TIM3 10kHz ISR 内调用（adc_app.c → HAL_TIM_PeriodElapsedCallback）：
 * - 每次调用都执行 OVP 检查（10kHz）
 * - 内部 10:1 分频，每 10 次执行一次 PI 计算（1kHz）
 * - 仅在 SAFE_RUN 状态执行 PI 计算（OVP 在 SOFTSTART+RUN 都生效）*/
void PI_CTRL_Step(void);

/* 获取诊断快照（主循环/串口打印用）。
 * 用 PRIMASK 临界区保护，保证读到一致的快照。
 * 传入的 snapshot 由调用方分配（栈上或静态区皆可）。*/
void PI_CTRL_GetDiagSnapshot(pi_ctrl_t *snapshot);

/* ============================================================================
 *  9. 编译期检查
 * ==========================================================================*/
/* 确保分段边界在有效范围内，且有合理的间隔 */
_Static_assert(PI_PERIOD_MIN < PI_PERIOD_SEG1,
               "PI_PERIOD_SEG1 must be > PI_PERIOD_MIN");
_Static_assert(PI_PERIOD_SEG1 < PI_PERIOD_SEG2,
               "PI_PERIOD_SEG1 must be < PI_PERIOD_SEG2");
_Static_assert(PI_PERIOD_SEG2 < PI_PERIOD_MAX,
               "PI_PERIOD_SEG2 must be < PI_PERIOD_MAX");
_Static_assert((PI_PERIOD_SEG2 - PI_PERIOD_SEG1) > (2.0f * PI_SEG_HYST),
               "SEG1~SEG2 gap must be > 2*HYST for hysteresis to work");
_Static_assert((PI_PERIOD_SEG1 - PI_PERIOD_MIN) > PI_SEG_HYST,
               "SEG1~MIN gap must be > HYST");

/* 确保保护阈值合理 */
_Static_assert(PI_VOUT_OVP_MV > PI_VOUT_TARGET_MV,
               "PI_VOUT_OVP_MV must be > PI_VOUT_TARGET_MV");
_Static_assert(PI_KI > 0.0f, "PI_KI must be > 0");
_Static_assert(PI_KP_HIGH > PI_KP_MID, "PI_KP_HIGH must be > PI_KP_MID");
_Static_assert(PI_KP_MID > PI_KP_LOW,  "PI_KP_MID must be > PI_KP_LOW");
_Static_assert(PI_DEADBAND_MV > 0.0f, "PI_DEADBAND_MV must be > 0");
_Static_assert(PI_SLEW_MAX > 0.0f,    "PI_SLEW_MAX must be > 0");
_Static_assert(PI_EWMA_ALPHA > 0.0f && PI_EWMA_ALPHA <= 1.0f,
               "PI_EWMA_ALPHA must be in (0, 1]");

#ifdef __cplusplus
}
#endif

#endif /* PI_CTRL_H */
