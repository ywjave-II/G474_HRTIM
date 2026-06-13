#include "freq_skip.h"


// 变量定义（只在这里定义一次，不加static因为.h中有extern）
volatile uint32_t llc_period = LLC_FREQ_START_PER;
volatile uint8_t  softstart_done = 0;

void LLC_SoftStart_Init(void)
{
    llc_period = LLC_FREQ_START_PER;
    softstart_done = 0;

    __HAL_HRTIM_SETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_MASTER,  llc_period);
    __HAL_HRTIM_SETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A, llc_period);
    __HAL_HRTIM_SETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_TIMER_C, llc_period);

    __HAL_HRTIM_MASTER_ENABLE_IT(&hhrtim1, HRTIM_MASTER_IT_MREP);

    HAL_HRTIM_WaveformOutputStart(&hhrtim1,
        HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
        HRTIM_OUTPUT_TC1 | HRTIM_OUTPUT_TC2);

    // HAL_HRTIM_WaveformCountStart(&hhrtim1,
    //     HRTIM_TIMERID_MASTER |
    //     HRTIM_TIMERID_TIMER_A |
    //     HRTIM_TIMERID_TIMER_C);

    //软启动开机时刻不开TIMER C 同步整流驱动。
    HAL_HRTIM_WaveformCountStart(&hhrtim1,
        HRTIM_TIMERID_MASTER |
        HRTIM_TIMERID_TIMER_A );

}

void LLC_SoftStart_Step(void)
{
    static uint16_t skip_cnt = 0;

    if (softstart_done) return;

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

        
        HRTIM1->sTimerxRegs[0].PERxR = llc_period;
        HRTIM1->sTimerxRegs[2].PERxR = llc_period;
        HRTIM1->sTimerxRegs[2].CMP1xR = llc_period / 2;       // 相位偏移跟随
        HRTIM1->sTimerxRegs[2].CMP4xR = llc_period - 342;     // 关断点跟随
        HRTIM1->sMasterRegs.MPER = llc_period;
    }
}