#include "pi_ctrl.h"
#include "hrtim.h"       /* hhrtim1 */

/* ============================================================================
 *  全局 PI 状态（仅此一处定义；extern 声明在 pi_ctrl.h）
 * ==========================================================================*/
volatile pi_ctrl_t g_pi = {0};

/* ============================================================================
 *  PI_CTRL_Init — 进入 RUN 状态时调用一次
 *  积分清零 + prev_error 清零 + 上次更新时刻清零。
 *  保证每次新进入 RUN（冷启动/热重启）都从干净起点开始调节。
 * ==========================================================================*/
void PI_CTRL_Init(void)
{
    g_pi.integral    = 0;
    g_pi.error       = 0;
    g_pi.prev_error  = 0;
    g_pi.p_term      = 0;
    g_pi.i_term      = 0;
    g_pi.delta_u     = 0;
}

/* ============================================================================
 *  写 HRTIM 周期寄存器（预装载 PERxR，下一 MREP 更新事件生效）
 *
 *  复用 LLC_SoftStart_Step() 的寄存器路径：MPER + TimerA/C PER + CMP1 + CMP4
 *  注意：TimerA CMP1 不写（其 InterleavedMode=DUAL 由硬件自动 PER/2）；
 *        TimerC CMP1=period/2 保持 180° 移相；CMP4=period-342 关断点。
 *
 *  ★ 写入顺序：先 PERxR(预装载)，最后 MPER(有效寄存器)，与已验证的
 *     LLC_SoftStart_Step() 保持一致。避免 MPER 先变而 TimerA/C 还是旧周期。
 *
 *  ★ CMP4 下溢保护：若 period ≤ 342（正常不会，PI_PERIOD_MIN=18133），
 *     CMP4 设为 1 而非 period-342，防止 uint32_t 回绕到接近 4e9 的大值。
 * ==========================================================================*/
static void PI_ApplyPeriod(uint32_t period)
{
    uint32_t cmp4;

    if (period > 342U) {
        cmp4 = period - 342U;
    } else {
        cmp4 = 1U;
    }

    /* 先预装载寄存器，最后写 Master 有效寄存器 */
    HRTIM1->sTimerxRegs[0].PERxR = period;                  /* Timer A */
    HRTIM1->sTimerxRegs[2].PERxR = period;                  /* Timer C */
    HRTIM1->sTimerxRegs[2].CMP1xR = period / 2U;            /* 180° 移相 */
    HRTIM1->sTimerxRegs[2].CMP4xR = cmp4;                   /* 关断点（防下溢）*/
    HRTIM1->sMasterRegs.MPER = period;                      /* Master 最后写 */
}

/* ============================================================================
 *  PI_CTRL_Step — TIM3 ISR 内 1kHz 分频调用，纯计算无阻塞
 *
 *  增量式 PI（velocity form），定点整数，无浮点开销：
 *
 *    delta_p = ((error - prev_error) * KP_INT) >> SHIFT    （增量 P）
 *    delta_i = (error * KI_INT) >> SHIFT                    （增量 I）
 *    delta_u = delta_p + delta_i
 *    period  += delta_u    （经 slew rate 限幅 + period 硬限幅）
 *
 *  与旧版（位置式）的关键区别：
 *    - P 项响应误差的「变化量」而非「绝对值」→ 消除"PI + 累加器"双重积分
 *    - I 项每周期独立计算，不依赖历史累加器 → 天然抗积分饱和
 *    - 积分累加器仅用于诊断/限幅，不直接参与输出计算
 * ==========================================================================*/
void PI_CTRL_Step(void)
{
    /* ---- 1. 误差计算 --------------------------------------------------- */
    uint16_t target_code = VOUT_MV_TO_CODE(PI_VOUT_TARGET_MV);
    int32_t  error       = (int32_t)target_code - (int32_t)g_vout_filt;
    g_pi.error           = error;

    /* ---- 2. 增量式 P：响应误差变化率（非误差绝对值）--------------------
     *     误差不变 → delta_p = 0，period 保持。只有误差变化才产生 P 输出，
     *     这消除了旧版"period=Σ(P+I)"中 P 项带来的额外积分效应。 */
    int32_t delta_p = ((error - g_pi.prev_error) * PI_KP_INT) >> PI_SHIFT;
    g_pi.p_term     = delta_p;

    /* ---- 3. 增量式 I：正比于当前误差（每周期独立计算）------------------- */
    int32_t delta_i = (error * PI_KI_INT) >> PI_SHIFT;

    /* ---- 4. Anti-Windup：输出已达限幅且误差方向仍推饱和 → 禁积分 -------
     *     同时冻结积分累加器，防止从饱和恢复时累加器已巨大 → 瞬间冲出。 */
    int windup_hi = ((int32_t)llc_period >= (int32_t)PI_PERIOD_MAX) && (error > 0);
    int windup_lo = ((int32_t)llc_period <= (int32_t)PI_PERIOD_MIN) && (error < 0);
    int frozen    = windup_hi || windup_lo;

    if (frozen)
    {
        delta_i = 0;          /* 禁止本次 I 增量 */
        /* 积分累加器也冻结，防止 windup 结束后瞬间释放大量 I */
    }
    else
    {
        g_pi.integral += error * PI_KI_INT;

        /* 积分限幅（Q8.8 量纲，与 PI_INTEGRAL_MAX/MIN_TICK 对齐）*/
        int32_t i_max = PI_INTEGRAL_MAX_TICK << PI_SHIFT;
        int32_t i_min = PI_INTEGRAL_MIN_TICK << PI_SHIFT;
        if (g_pi.integral > i_max) { g_pi.integral = i_max; }
        if (g_pi.integral < i_min) { g_pi.integral = i_min; }
    }

    g_pi.i_term = delta_i;

    /* ---- 5. 总增量 ----------------------------------------------------- */
    int32_t delta_u = delta_p + delta_i;

    /* ---- 6. Slew rate 限制：单次变化不超过 ±PI_PERIOD_SLEW_MAX ---------
     *     防止单拍周期突变过大（如 ADC 毛刺导致大 error 跳变）。 */
    if (delta_u > (int32_t)PI_PERIOD_SLEW_MAX)
    {
        delta_u = (int32_t)PI_PERIOD_SLEW_MAX;
    }
    else if (delta_u < -((int32_t)PI_PERIOD_SLEW_MAX))
    {
        delta_u = -((int32_t)PI_PERIOD_SLEW_MAX);
    }

    g_pi.delta_u = delta_u;

    /* ---- 7. 周期更新 + 硬限幅 ----------------------------------------- */
    int32_t new_period = (int32_t)llc_period + delta_u;

    if (new_period < (int32_t)PI_PERIOD_MIN)
    {
        new_period = (int32_t)PI_PERIOD_MIN;
    }
    else if (new_period > (int32_t)PI_PERIOD_MAX)
    {
        new_period = (int32_t)PI_PERIOD_MAX;
    }

    llc_period = (uint32_t)new_period;

    /* ---- 8. 写入 HRTIM 预装载寄存器（下一 MREP 生效）------------------- */
    PI_ApplyPeriod(llc_period);

    /* ---- 9. 保存本次误差供下一周期增量 P 计算 ------------------------- */
    g_pi.prev_error = error;
}
