#include "pi_ctrl.h"
#include "safe_sm.h"   /* g_safe_state, SAFE_RUN, SAFE_SOFTSTART */
#include "hrtim.h"     /* hhrtim1, HAL_HRTIM_WaveformOutputStop */
#include "adc_app.h"   /* ADC_APP_ENABLE_VOUT — 与 buffer 定义保持同步 */

/* ============================================================================
 *  全局 PI 状态（仅此一处定义；extern 声明在 pi_ctrl.h）
 * ==========================================================================*/
volatile pi_ctrl_t g_pi = {0};
volatile uint8_t   g_fault_request = 0;

/* ADC DMA buffer — 由 adc_app.c 定义并启动 DMA，此处仅引用。
 * ADC2 扫描序列：Rank0=VOUT(CH12)=g_adc_dma_buf[0], Rank1=IOUT(CH5)=g_adc_dma_buf[1] */
extern uint16_t g_adc_dma_buf[];

/* ============================================================================
 *  模块静态变量
 * ==========================================================================*/
static uint8_t  s_ewma_primed = 0;    /* EWMA 首样预置标志，Init 时清零 */
static uint8_t  s_pi_tick     = 0;    /* 10kHz→1kHz 分频计数器 */

/* ============================================================================
 *  PI_GetKp — 分段 Kp 查表（带滞回）
 *  --------------------------------------------------------------------------
 *  使用 g_pi.kp_segment 记录当前所在段，在边界两侧各留 PI_SEG_HYST 的滞回带，
 *  防止 period 在边界附近来回穿越时 Kp 频繁切换（chattering）。
 *
 *  滞回规则：
 *    HIGH → MID：period > SEG1 + HYST（远离边界才降段）
 *    MID  → HIGH：period < SEG1 - HYST（远离边界才升段）
 *    MID  → LOW： period > SEG2 + HYST
 *    LOW  → MID： period < SEG2 - HYST
 * ==========================================================================*/
static float PI_GetKp(float period)
{
    switch (g_pi.kp_segment)
    {
        case PI_SEG_HIGH:
            /* 当前在高频段：只有当 period 明显超过 SEG1 才降到中频 */
            if (period > (PI_PERIOD_SEG1 + PI_SEG_HYST))
            {
                g_pi.kp_segment = PI_SEG_MID;
                return PI_KP_MID;
            }
            return PI_KP_HIGH;

        case PI_SEG_MID:
            /* 当前在中频段：需明显低于 SEG1 才升到高频，明显超过 SEG2 才降到低频 */
            if (period < (PI_PERIOD_SEG1 - PI_SEG_HYST))
            {
                g_pi.kp_segment = PI_SEG_HIGH;
                return PI_KP_HIGH;
            }
            if (period > (PI_PERIOD_SEG2 + PI_SEG_HYST))
            {
                g_pi.kp_segment = PI_SEG_LOW;
                return PI_KP_LOW;
            }
            return PI_KP_MID;

        case PI_SEG_LOW:
            /* 当前在低频段：只有当 period 明显低于 SEG2 才升到中频 */
            if (period < (PI_PERIOD_SEG2 - PI_SEG_HYST))
            {
                g_pi.kp_segment = PI_SEG_MID;
                return PI_KP_MID;
            }
            return PI_KP_LOW;

        default:
            /* 异常兜底 */
            g_pi.kp_segment = PI_SEG_MID;
            return PI_KP_MID;
    }
}

/* ============================================================================
 *  PI_CTRL_Init — 进入 RUN 状态时调用一次
 *  --------------------------------------------------------------------------
 *  清零控制状态（积分/误差），重置 EWMA 首样标志，误差峰值归零。
 *  累计统计（slew_clip / windup / ovp）跨 RUN 保留，便于长期诊断。
 * ==========================================================================*/
void PI_CTRL_Init(void)
{
    g_pi.vout_filt   = 0.0f;
    g_pi.error       = 0.0f;
    g_pi.error_max   = 0.0f;
    g_pi.prev_error  = 0.0f;
    g_pi.delta_p     = 0.0f;
    g_pi.delta_i     = 0.0f;
    g_pi.delta_u     = 0.0f;
    g_pi.integral    = 0.0f;
    g_pi.vout_raw    = 0;
    g_pi.kp_segment  = PI_SEG_MID;   /* 软启动终点约 135kHz(period≈40300)，在中频段 */

    s_ewma_primed    = 0;
    s_pi_tick        = 0;

    /* slew_clip_count / windup_count / ovp_count 不在此清零 */
}

/* ============================================================================
 *  PI_CTRL_Step — TIM3 10kHz ISR 内调用
 *  --------------------------------------------------------------------------
 *  执行顺序（15 步）：
 *    Step  0: OVP 检查（每次 10kHz 都执行，覆盖 SOFTSTART + RUN）
 *    Step  1: 计数器分频判断（不足 1ms 返回）
 *    Step  2: ADC 读取（直接读 DMA buffer）
 *    Step  3: EWMA 低通滤波
 *    Step  4: ADC 码 → mV 换算
 *    Step  5: 误差计算
 *    Step  6: 死区判断（命中则更新 prev_error 后返回）
 *    Step  7: 分段 Kp 查表（带滞回）
 *    Step  8: 增量 P 计算（响应误差变化率）
 *    Step  9: 增量 I 计算（消除稳态误差）
 *    Step 10: Anti-Windup 判断（饱和时禁 delta_i）
 *    Step 11: 合并 delta_u = delta_p + delta_i
 *    Step 12: Slew rate 限幅
 *    Step 13: period 更新 + 硬限幅
 *    Step 14: 写 HRTIM 预装载寄存器
 *    Step 15: 更新 prev_error 供下拍使用
 * ==========================================================================*/
void PI_CTRL_Step(void)
{
    /* ========================================================================
     *  Step 0: OVP 检查（每次 10kHz 都执行）
     *  覆盖 SOFTSTART + RUN 全状态，不依赖 1kHz 分频。
     *  触发时：计数 + 置 g_fault_request 标志 + 立即关 HRTIM 输出（硬快路径），
     *  主循环 SafeSM_Poll 检测到标志后调用 SafeSM_EnterFault() 完成状态转移。
     *  注意：EWMA 首样未就绪时（vout_filt==0）跳过，避免误触发。
     * ========================================================================*/
    safe_state_t st = g_safe_state;   /* volatile 快照 */
    if ((st == SAFE_SOFTSTART || st == SAFE_RUN)
        && (g_pi.vout_filt > PI_VOUT_OVP_MV)
        && (g_pi.vout_filt > 0.0f))
    {
        g_pi.ovp_count++;
        g_fault_request = 1;

        /* 直接关 HRTIM 输出 — 不等待主循环，纳秒级响应。
         * SafeSM_EnterFault() 由主循环检测 g_fault_request 后调用（补 DIS + 状态转移）。*/
        HAL_HRTIM_WaveformOutputStop(&hhrtim1,
            HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
            HRTIM_OUTPUT_TC1 | HRTIM_OUTPUT_TC2);
        return;
    }

    /* ========================================================================
     *  Step 1: 计数器分频判断（10kHz → 1kHz）
     *  不足 1ms 时直接返回，仅执行了上面的 OVP 检查。
     * ========================================================================*/
    if (++s_pi_tick < PI_DECIMATION)
    {
        return;
    }
    s_pi_tick = 0;

    /* ========================================================================
     *  Step 2: ADC 读取 — 直接读 DMA buffer
     *  VOUT 在 g_adc_dma_buf[0]（ADC2 规则组，DMA 循环模式）。
     * ========================================================================*/
    uint16_t raw = g_adc_dma_buf[0];
    g_pi.vout_raw = raw;

    /* ========================================================================
     *  Step 3: EWMA 低通滤波
     *  filt = α·raw + (1-α)·prev_filt
     *  首样直接预置，避免从 0 爬升期间误判。
     * ========================================================================*/
    float raw_mv = (float)raw * PI_VOUT_SCALE;

    if (!s_ewma_primed)
    {
        g_pi.vout_filt  = raw_mv;
        s_ewma_primed   = 1;
    }
    else
    {
        g_pi.vout_filt = PI_EWMA_ALPHA * raw_mv
                       + (1.0f - PI_EWMA_ALPHA) * g_pi.vout_filt;
    }

    /* ========================================================================
     *  Step 4: ADC 码 → mV 换算（已在 Step 3 完成，此处仅保留分步注释）
     *  raw_mv = raw * PI_VOUT_SCALE，其中 PI_VOUT_SCALE ≈ 8.864 mV/码。
     * ========================================================================*/

    /* ========================================================================
     *  Step 5: 误差计算
     *  error = target - vout_filt（mV）
     *  正误差 = VOUT 偏低 → 需增 period（降频增增益）
     *  负误差 = VOUT 偏高 → 需减 period（升频降增益）
     * ========================================================================*/
    float error = PI_VOUT_TARGET_MV - g_pi.vout_filt;
    g_pi.error  = error;

    /* 更新历史最大绝对误差（诊断用）*/
    float abs_err = (error < 0.0f) ? -error : error;   /* 不用 fabsf */
    if (abs_err > g_pi.error_max)
    {
        g_pi.error_max = abs_err;
    }

    /* ========================================================================
     *  Step 6: 死区判断
     *  |error| < DEADBAND_MV → 不更新 period，但仍更新 prev_error。
     *  关键：死区内持续更新 prev_error，防止退出死区时 delta_p 产生冲击
     *  （旧 error 与 new error 的差值被 deadband 期间的漂移放大）。
     * ========================================================================*/
    if (abs_err < PI_DEADBAND_MV)
    {
        g_pi.prev_error = error;    /* 死区内仍更新 prev_error */
        g_pi.delta_p    = 0.0f;
        g_pi.delta_i    = 0.0f;
        g_pi.delta_u    = 0.0f;
        return;                     /* 不更新 period */
    }

    /* ========================================================================
     *  Step 7: 分段 Kp 查表（带滞回）
     *  根据当前 llc_period 选择 Kp，滞回防止边界 chattering。
     * ========================================================================*/
    float period_f = (float)llc_period;
    float kp = PI_GetKp(period_f);

    /* ========================================================================
     *  Step 8: 增量 P 计算
     *  delta_p = (error - prev_error) * Kp
     *  仅响应误差的「变化量」：误差不变 → delta_p = 0 → period 冻住。
     *  这消除了位置式 PI 中 period 累积器 + P 项绝对值叠加的"双重积分"问题。
     *  单位：tick = mV × (tick/mV)
     * ========================================================================*/
    float delta_p = (error - g_pi.prev_error) * kp;
    g_pi.delta_p  = delta_p;

    /* ========================================================================
     *  Step 9: 增量 I 计算
     *  delta_i = error * Ki
     *  每拍独立计算，不累积历史。浮点实现无截断，小误差也能产生非零 I 增量。
     *  单位：tick = mV × tick/(mV·ms)，控制周期固定 1ms 已隐含在 Ki 中。
     * ========================================================================*/
    float delta_i = error * PI_KI;
    g_pi.delta_i  = delta_i;

    /* 积分累加器仅用于诊断记录（不参与控制输出）*/
    g_pi.integral += delta_i;

    /* ========================================================================
     *  Step 10: Anti-Windup — 饱和时禁 delta_i
     *  正确判断：period 已达限幅 且 error 方向仍将其推向该限幅 → 禁 delta_i。
     *  使用本次计算前的 llc_period（允许一拍滞后，可接受）。
     *  增量式 PI 的 period 本身是唯一的累积器，无需额外的积分累加器参与输出。
     * ========================================================================*/
    int windup_hi = (period_f >= PI_PERIOD_MAX) && (error > 0.0f);
    int windup_lo = (period_f <= PI_PERIOD_MIN) && (error < 0.0f);

    if (windup_hi || windup_lo)
    {
        delta_i = 0.0f;
        g_pi.delta_i = 0.0f;
        g_pi.windup_count++;
    }

    /* ========================================================================
     *  Step 11: 合并 delta_u
     * ========================================================================*/
    float delta_u = delta_p + delta_i;

    /* ========================================================================
     *  Step 12: Slew rate 限幅 — 防单拍突变
     *  单位：tick/次。不依赖 HAL float 函数，直接用条件判断。
     * ========================================================================*/
    if (delta_u > PI_SLEW_MAX)
    {
        delta_u = PI_SLEW_MAX;
        g_pi.slew_clip_count++;
    }
    else if (delta_u < -PI_SLEW_MAX)
    {
        delta_u = -PI_SLEW_MAX;
        g_pi.slew_clip_count++;
    }

    g_pi.delta_u = delta_u;

    /* ========================================================================
     *  Step 13: period 更新 + 硬限幅
     * ========================================================================*/
    float new_period = period_f + delta_u;

    if (new_period < PI_PERIOD_MIN)
    {
        new_period = PI_PERIOD_MIN;
    }
    else if (new_period > PI_PERIOD_MAX)
    {
        new_period = PI_PERIOD_MAX;
    }

    llc_period = (uint32_t)new_period;

    /* ========================================================================
     *  Step 14: 写 HRTIM 预装载寄存器（下一 MREP 更新事件生效）
     *  通过统一的 HRTIM_SetLLCPeriod()（定义在 freq_skip.c 中）。
     *  该函数包含 CMP4 下溢保护 + 寄存器写入顺序保证。
     * ========================================================================*/
    HRTIM_SetLLCPeriod(llc_period);

    /* ========================================================================
     *  Step 15: 保存本次误差供下拍增量 P 计算
     * ========================================================================*/
    g_pi.prev_error = error;
}

/* ============================================================================
 *  PI_CTRL_GetDiagSnapshot — 获取诊断快照（主循环/串口打印用）
 *  --------------------------------------------------------------------------
 *  用 PRIMASK 临时关全局中断，保证读到的快照原子一致（不被 ISR 半途修改）。
 *  G474 上 float 是单指令加载/存储（硬件 FPU），但结构体整体拷贝临界区最安全。
 * ==========================================================================*/
void PI_CTRL_GetDiagSnapshot(pi_ctrl_t *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();                        /* 关全局中断 */

    *snapshot = *(const pi_ctrl_t *)&g_pi;  /* volatile → 栈拷贝 */

    if (!primask)
    {
        __enable_irq();                     /* 仅在原本开中断时才恢复 */
    }
    /* 原本就关中断则保持关（嵌套 ISR 安全）*/
}
