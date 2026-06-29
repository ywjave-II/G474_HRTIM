#include "burst_mode.h"
#include "hrtim.h"       /* hhrtim1, HRTIM_OUTPUT_TA1/TA2 */
#include "freq_skip.h"   /* HRTIM_SetLLCPeriod, llc_period */
#include "pi_ctrl.h"     /* PI_VOUT_SCALE：mV→ADC码换算 */
#include "adc_app.h"     /* g_iout_ma */

/* ADC DMA buffer — 由 adc_app.c 定义并启动 DMA，此处仅引用。
 * ADC2 扫描序列：Rank0=VOUT(CH12)=g_adc_dma_buf[0], Rank1=IOUT(CH5)=g_adc_dma_buf[1] */
extern uint16_t g_adc_dma_buf[];

/* TA1+TA2 输出位掩码（ODISR / OENR 寄存器直接操作）*/
#define BURST_OUT_MASK  (HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2)

/* ============================================================================
 *  运行时标定变量（默认值从宏初始化，后续可通过串口指令修改）
 * ==========================================================================*/
volatile int32_t  g_burst_iout_enter_ma = BURST_IOUT_ENTER_MA_DEF;
volatile int32_t  g_burst_iout_exit_ma  = BURST_IOUT_EXIT_MA_DEF;
volatile int32_t  g_burst_vout_on_mv    = BURST_VOUT_ON_MV_DEF;
volatile int32_t  g_burst_vout_off_mv   = BURST_VOUT_OFF_MV_DEF;

/* ---- 诊断计数器（跨 BURST 进入/退出累计，仅冷启清零）---- */
volatile uint32_t g_burst_on_count  = 0;
volatile uint32_t g_burst_off_count = 0;

/* ============================================================================
 *  模块静态变量
 * ==========================================================================*/

/* 预计算的 ADC 码门限（BURST_Init 时从 mV 门限换算，ISR 中直接整数比较）*/
static uint16_t s_burst_on_code  = 0;   /* VOUT < 此码 → burst on */
static uint16_t s_burst_off_code = 0;   /* VOUT > 此码 → burst off */

/* PWM 输出当前状态：1=发波(TA1/TA2 使能), 0=停波(ODISR 禁用) */
static uint8_t s_burst_output_on = 0;

/* VOUT 滞回去抖计数器（ISR 中 1kHz 累加，连续同向 N 次才切换）*/
static uint8_t s_vout_high_cnt = 0;     /* 连续高于 OFF 门限的次数 */
static uint8_t s_vout_low_cnt  = 0;     /* 连续低于 ON  门限的次数 */

/* IOUT 去抖计时（主循环 HAL_GetTick 模式）*/
static uint8_t  s_enter_timing  = 0;    /* Channel 1: IOUT < enter 门限 */
static uint32_t s_enter_t0      = 0;
static uint8_t  s_exit_timing   = 0;    /* 退出：IOUT > exit 门限 */
static uint32_t s_exit_t0       = 0;

/* ============================================================================
 *  BURST_Init — 进入 Burst 状态时调用一次
 * ==========================================================================*/
void BURST_Init(void)
{
    /* 把 mV 门限换算为 ADC 码门限（仅 Init 时浮点运算一次，ISR 中纯整数比较）。
     * code = mV / PI_VOUT_SCALE，四舍五入。
     * PI_VOUT_SCALE ≈ 8.864 mV/码 (3300/4095 * 11/1) */
    s_burst_on_code  = (uint16_t)((float)g_burst_vout_on_mv  / PI_VOUT_SCALE + 0.5f);
    s_burst_off_code = (uint16_t)((float)g_burst_vout_off_mv / PI_VOUT_SCALE + 0.5f);

    /* 设 Burst 固定周期并写入 HRTIM */
    llc_period = BURST_PERIOD;
    HRTIM_SetLLCPeriod(BURST_PERIOD);

    /* 确保 TA1/TA2 输出使能（从 RUN 进入时本就使能，这里幂等确认）。
     * 写 OENR 使能输出——等价于 HAL_HRTIM_WaveformOutputStart 的底层操作，
     * 但不经 HAL 锁，ISR 安全。*/
    HRTIM1->sCommonRegs.OENR |= BURST_OUT_MASK;
    s_burst_output_on = 1;

    /* 复位全部去抖状态 */
    s_vout_high_cnt = 0;
    s_vout_low_cnt  = 0;
    s_enter_timing  = 0;
    s_exit_timing   = 0;
}

/* ============================================================================
 *  BURST_FastEnter — ISR 快速进入（TIM3 10kHz 上下文）
 *  --------------------------------------------------------------------------
 *  与 BURST_Init 的区别：
 *    - ODISR 立即关 PWM（VOUT 已超门限，不能再发波）
 *    - 其他初始化相同（ADC 码门限 / Burst 周期 / 去抖复位）
 *    - ISR 安全：不调用 HAL，直接写寄存器
 * ==========================================================================*/
void BURST_FastEnter(void)
{
    s_burst_on_code  = (uint16_t)((float)g_burst_vout_on_mv  / PI_VOUT_SCALE + 0.5f);
    s_burst_off_code = (uint16_t)((float)g_burst_vout_off_mv / PI_VOUT_SCALE + 0.5f);

    llc_period = BURST_PERIOD;
    HRTIM_SetLLCPeriod(BURST_PERIOD);

    /* VOUT 已超 off 门限 → 立即关输出，不等 BURST_Step 去抖 */
    HRTIM1->sCommonRegs.ODISR = BURST_OUT_MASK;
    s_burst_output_on = 0;
    g_burst_off_count++;

    s_vout_high_cnt = 0;
    s_vout_low_cnt  = 0;
    s_enter_timing  = 0;
    s_exit_timing   = 0;
}

/* ============================================================================
 *  BURST_Step — TIM3 ISR 中 1kHz 调用
 *  --------------------------------------------------------------------------
 *  直接读 ADC DMA buffer 原始码 → 整数滞回比较 → ODISR/OENR 门控 PWM。
 *  ODISR (Output Disable Set Register):   写 1 的位 → 禁用对应输出
 *  OENR  (Output Enable Register):        写 1 的位 → 使能对应输出
 *  这两个写操作等价于 HAL_HRTIM_WaveformOutputStop/Start 的底层操作，
 *  但不经 HAL 锁，不检查 State，纯寄存器级，ISR 安全。
 *
 *  效果：Timer A 计数器、比较器、死区单元全程照常运行，只在输出驱动级
 *        使能/禁用 TA1/TA2。恢复时立即无缝接续 PWM。
 * ==========================================================================*/
void BURST_Step(void)
{
    uint16_t raw = g_adc_dma_buf[0];   /* VOUT 原始 ADC 码（DMA 循环更新，≤100µs 旧）*/

    if (s_burst_output_on)
    {
        /* PWM 正在输出：检测 VOUT 是否过高 → 禁用输出 */
        if (raw > s_burst_off_code)
        {
            s_vout_high_cnt++;
            if (s_vout_high_cnt >= BURST_VOUT_DEBOUNCE_CNT)
            {
                s_vout_high_cnt = 0;
                /* 禁用 TA1/TA2 输出 → PA8/PA9 拉到 INACTIVE（低电平）*/
                HRTIM1->sCommonRegs.ODISR = BURST_OUT_MASK;
                s_burst_output_on = 0;
                g_burst_off_count++;
            }
        }
        else
        {
            s_vout_high_cnt = 0;   /* 未持续高于门限 → 重置 */
        }
    }
    else
    {
        /* PWM 已停波：检测 VOUT 是否过低 → 重新使能输出 */
        if (raw < s_burst_on_code)
        {
            s_vout_low_cnt++;
            if (s_vout_low_cnt >= BURST_VOUT_DEBOUNCE_CNT)
            {
                s_vout_low_cnt = 0;

                /* 确保周期仍为 Burst 频率（可能被其他路径修改）*/
                llc_period = BURST_PERIOD;
                HRTIM_SetLLCPeriod(BURST_PERIOD);

                /* 使能 TA1/TA2 输出 → 立即恢复 PWM */
                HRTIM1->sCommonRegs.OENR |= BURST_OUT_MASK;
                s_burst_output_on = 1;
                g_burst_on_count++;
            }
        }
        else
        {
            s_vout_low_cnt = 0;    /* 未持续低于门限 → 重置 */
        }
    }
}

/* ============================================================================
 *  BURST_ShouldEnter — 主循环 SafeSM_Poll RUN 分支调用
 *  --------------------------------------------------------------------------
 *  双通道进入，任一通道持续 5ms 即触发：
 *
 *   Channel 1（电流基）：IOUT < enter 门限（默认 100mA）→ 典型轻载/空载触发
 *   Channel 2（电压基）：VOUT > off 门限（默认 24.5V）且 IOUT < exit 门限（默认 400mA）
 *                       → 中轻载电压爬升时兜底，避免 PI 半饱和无力降压
 *
 *   Channel 2 的电流上限确认（< 400mA）是关键：防止大载动态瞬间超调
 *   导致误入 Burst（大载下应让 PI 继续调节，而非间歇停波）。
 * ==========================================================================*/
int BURST_ShouldEnter(void)
{
    /* ---- Channel 1：IOUT 持续低于进入门限 ---- */
    if (g_iout_ma < g_burst_iout_enter_ma)
    {
        if (!s_enter_timing)
        {
            s_enter_timing = 1;
            s_enter_t0     = HAL_GetTick();
        }
        else if ((HAL_GetTick() - s_enter_t0) >= BURST_DEBOUNCE_MS)
        {
            s_enter_timing = 0;
            return 1;
        }
    }
    else
    {
        s_enter_timing = 0;
    }

    /* ---- Channel 2：VOUT 过高 + 电流确认不大 → 立即响应，无去抖 ----
     * VOUT 已超过 Burst off 门限（默认 24.5V），多等 1ms 就多窜一截电压。
     * 电流 < exit 门限（默认 400mA）已确认不是大载动态超调，直接进入 Burst。
     * 进入后的 PWM 门控本身有 2 次去抖（BURST_Step），不会因单次噪声误动作。*/
    if (g_pi.vout_filt > (float)g_burst_vout_off_mv && g_iout_ma < g_burst_iout_exit_ma)
    {
        return 1;
    }

    return 0;
}

/* ============================================================================
 *  BURST_ShouldExit — 主循环 SafeSM_Poll BURST 分支调用
 * ==========================================================================*/
int BURST_ShouldExit(void)
{
    if (g_iout_ma > g_burst_iout_exit_ma)
    {
        if (!s_exit_timing)
        {
            s_exit_timing = 1;
            s_exit_t0     = HAL_GetTick();
        }
        else if ((HAL_GetTick() - s_exit_t0) >= BURST_DEBOUNCE_MS)
        {
            s_exit_timing = 0;
            return 1;   /* 确认退出 Burst */
        }
    }
    else
    {
        s_exit_timing = 0;    /* 电流回落，计时作废 */
    }
    return 0;
}
