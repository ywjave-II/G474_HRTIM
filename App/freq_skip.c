#include "freq_skip.h"
#include "adc_app.h"
#include <stdint.h>

// 变量定义（只在这里定义一次，不加static因为.h中有extern）
volatile uint32_t llc_period = LLC_FREQ_START_PER;
volatile uint32_t llc_period_frzee =0;
volatile uint8_t  softstart_done = 0;
/* ADC DMA buffer — 由 adc_app.c 定义并启动 DMA，此处仅引用。
 * ADC2 扫描序列：Rank0=VOUT(CH12)=g_adc_dma_buf[0], Rank1=IOUT(CH5)=g_adc_dma_buf[1] */
extern uint16_t g_adc_dma_buf[];
void LLC_SoftStart_Init(void)
{
    /* (1) 复位扫频状态到 300k 起点（冷启动 / re-arm 共用此入口）。*/
    llc_period     = LLC_FREQ_START_PER;
    softstart_done = 0;

    /* 先停计数器，拿到与冷启动一致的确定起点。
     * 关键：re-arm 路径下 MCU 未掉电、HRTIM 未复位，关断前 RUN 定频(~140k)的 period 仍残留在
     * 有效寄存器里，且 SafeSM_EnterFault() 只停了输出、计数器还在以旧周期空跑。若直接写预装载就
     * 使能输出，旧 140k 会在更新事件(MREP，最多 4 拍)前漏出几拍。故此处停计数 + 后面强制 latch。*/
    HAL_HRTIM_WaveformCountStop(&hhrtim1,
        HRTIM_TIMERID_MASTER | HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_C);

    /* (2) 把 300k 起始周期写入预装载寄存器。*/
    __HAL_HRTIM_SETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_MASTER,  llc_period);
    __HAL_HRTIM_SETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A, llc_period);
    __HAL_HRTIM_SETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_TIMER_C, llc_period);

    /* (3) 先把计数器清零，再强制「预装载 -> 有效寄存器」立即生效。
     *     顺序关键：在 CNT=0 的干净点做 SoftwareUpdate，使 period 与 TimerA interleaved 自动
     *     CMP1=PER/2 在【同一更新事件】里一起 latch。否则首拍 CMP1 可能仍是旧 PER/2(≈19550)，
     *     而新周期只有 18133<19550 → TA1 第一周期内等不到复位点 → 首拍高电平拖到次拍(≈3 倍宽)。LLC_FREQ_START_PER
     *     HAL 文档原义："Force an immediate transfer from the preload to the active register"。*/
    HAL_HRTIM_SoftwareReset(&hhrtim1,
        HRTIM_TIMERRESET_MASTER | HRTIM_TIMERRESET_TIMER_A | HRTIM_TIMERRESET_TIMER_C);
    HAL_HRTIM_SoftwareUpdate(&hhrtim1,
        HRTIM_TIMERUPDATE_MASTER | HRTIM_TIMERUPDATE_A | HRTIM_TIMERUPDATE_C);

    /* 确认 latch 真正完成：SWU 位由硬件在搬运完成后自清。
     * 有界兜底：正常几个时钟即清；给足余量后超时退出，避免万一不自清时主循环死等
     * （无 IWDG，死等会让板子带 PWM 僵住）。*/
    {
        uint32_t swu_guard = 10000U;
        while ((HRTIM1->sCommonRegs.CR2 &
                (HRTIM_CR2_MSWU | HRTIM_CR2_TASWU | HRTIM_CR2_TCSWU)) &&
               (--swu_guard != 0U))
        {
        }
    }

    /* (3b) 使能输出前，把主通道 TA1 强制到失活电平，清掉上次 RUN 残留的 SR 锁存态，
     *      保证起振首个上升沿是干净的 MASTERPER 置位（互补的 TA2 由死区单元从主通道派生，
     *      死区开启时无法、也无需单独强制）。消除"偶发首拍偏宽"的残留竞态。
     *      注：TimerC（副边同步整流）本版开环测试不驱动，输出不使能（见下），故不在此强制。*/
    HAL_HRTIM_WaveformSetOutputLevel(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
        HRTIM_OUTPUT_TA1, HRTIM_OUTPUTLEVEL_INACTIVE);

    __HAL_HRTIM_MASTER_ENABLE_IT(&hhrtim1, HRTIM_MASTER_IT_MREP);

    /* (4) 起始 period+CMP 已生效、输出已清到失活后，才解除封波 / 使能输出 + 启动计数。
     *     ⚠️ 只使能原边 TA1/TA2。TimerC（副边同步整流 TC1/TC2）本版不计数，若在此使能输出，
     *     死区单元会把停在失活态的 TC1 取反 → TC2(PB13) 被钉死在高电平（假驱动）。要做同步整流
     *     时应改为同时 CountStart(TIMER_C) 并在此放开 TC1/TC2，而非只使能输出。*/
    HAL_HRTIM_WaveformOutputStart(&hhrtim1,
        HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2);

    HAL_HRTIM_WaveformCountStart(&hhrtim1,
        HRTIM_TIMERID_MASTER |
        HRTIM_TIMERID_TIMER_A );
}

void LLC_SoftStart_Step(void)
{
    static uint16_t skip_cnt = 0;

    if (softstart_done) return;
    
    
    if (g_adc_dma_buf[0]>SOFTSTAR_EXIT_ADC_CODE) {
        softstart_done = 1;
        skip_cnt = 0;
        llc_period_frzee=llc_period;
        return; // 提前退出，等待下一拍状态机无缝切换至 RUN(PI控制)
    }
    skip_cnt++;
    if (skip_cnt >= LLC_SKIP_COUNT)
    {
        skip_cnt = 0;
        llc_period += LLC_SOFTSTART_STEP;

        if (llc_period >= LLC_FREQ_TARGET_PER)
        {
            llc_period = LLC_FREQ_TARGET_PER;
            softstart_done = 1;
        }

        HRTIM_SetLLCPeriod(llc_period);
    }
}

/* ============================================================================
 *  HRTIM_SetLLCPeriod — 统一的 HRTIM 周期寄存器写入
 *  --------------------------------------------------------------------------
 *  供 PI 闭环（pi_ctrl.c）和软启动（LLC_SoftStart_Step）共同调用。
 *
 *  写入顺序（已验证，勿改）：
 *    1. TimerA PERxR（预装载）
 *    2. TimerC PERxR（预装载）
 *    3. TimerC CMP1 = period/2（180° 移相）
 *    4. TimerC CMP4 = period - PI_CMP4_OFFSET（关断点，有下溢保护）
 *    5. Master MPER（最后写有效寄存器）
 *
 *  注意：TimerA CMP1 不写 — InterleavedMode=DUAL 由硬件自动设为 PER/2。
 *        预装载值在下一 MREP 更新事件时自动搬运到 active 寄存器。
 *
 *  CMP4 下溢保护：正常 period > PI_CMP4_OFFSET（最小值 18133 >> 342）。
 *  若 period ≤ offset，CMP4 设为 1，防止 uint32_t 回绕到接近 4e9。
 * ==========================================================================*/
void HRTIM_SetLLCPeriod(uint32_t period)
{
    uint32_t cmp4;

    if (period > PI_CMP4_OFFSET)
    {
        cmp4 = period - PI_CMP4_OFFSET;
    }
    else
    {
        cmp4 = 1U;   /* 下溢保护：防 uint32_t 回绕 */
    }

    /* 先写预装载寄存器（PERxR / CMPxR），最后写 Master 有效寄存器（MPER）*/
    HRTIM1->sTimerxRegs[0].PERxR = period;       /* Timer A */
    HRTIM1->sTimerxRegs[2].PERxR = period;       /* Timer C */
    HRTIM1->sTimerxRegs[2].CMP1xR = period / 2U; /* 180° 移相 */
    HRTIM1->sTimerxRegs[2].CMP4xR = cmp4;        /* 关断点（下溢保护）*/
    HRTIM1->sMasterRegs.MPER = period;           /* Master 最后写 */
}