#include "fault_log.h"
#include "freq_skip.h"     /* llc_period / softstart_done，用于打印实际开关频率 */
#include "adc_app.h"       /* g_vaux_raw / g_vaux_filt / g_vaux_mv，用于打印 VAUX 采样；
                             * 若启用 VOUT/I_CYCLE/IOU，对应全局变量也在此提供 */
#include "safe_sm.h"       /* g_safe_state，打印安全状态机当前状态 */
#include "pi_ctrl.h"        /* g_pi，打印 PI 闭环误差/P/I 项 */
#include <stdio.h>         /* printf -> USART1（io_retarget.c 重定向）*/

/* 等效计数时钟：HRTIM MUL32，5440 MHz。fsw = 5440e6 / period */
#define HRTIM_EQUIV_CLK_HZ   5440000000ULL

/* 故障记录全局实例（只在此定义一次）*/
volatile fault_record_t g_fault = {0};

#define FLT_IT_ALL    (HRTIM_IT_FLT1   | HRTIM_IT_FLT2   | HRTIM_IT_FLT3)
#define FLT_FLAG_ALL  (HRTIM_FLAG_FLT1 | HRTIM_FLAG_FLT2 | HRTIM_FLAG_FLT3)

/* 故障指示灯：LED1/2/3 = PC1/PC2/PC3，开漏，低电平(RESET)点亮 */
#define FAULT_LED_ON(port, pin)   HAL_GPIO_WritePin((port), (pin), GPIO_PIN_RESET)
#define FAULT_LED_OFF(port, pin)  HAL_GPIO_WritePin((port), (pin), GPIO_PIN_SET)

void Fault_IRQ_Enable(void)
{
    /* 基线：熄灭三路故障灯（gpio.c 上电默认把 LED 拉低=点亮，需在此复位），
     * 之后 "某灯点亮" 即代表对应通道故障已触发 */
    FAULT_LED_OFF(LED1_GPIO_Port, LED1_Pin);
    FAULT_LED_OFF(LED2_GPIO_Port, LED2_Pin);
    FAULT_LED_OFF(LED3_GPIO_Port, LED3_Pin);

    /* 先清 ISR 标志，避免上电/启动期间的历史误触发一开中断就立刻进 ISR */
    __HAL_HRTIM_CLEAR_FLAG(&hhrtim1, FLT_FLAG_ALL);

    /* 工程使用 HAL_HRTIM_WaveformCountStart（非 _IT 版），IER 不会被自动写入，
     * 故此处显式使能 FLT1/2/3 中断（与 freq_skip 里手动开 MREP 中断同样的做法）。
     * NVIC（HRTIM1_FLT_IRQn）已由 CubeMX 在 HAL_HRTIM_MspInit 中开启，此处无需重复。*/
    __HAL_HRTIM_ENABLE_IT(&hhrtim1, FLT_IT_ALL);
}

void Fault_OnIRQ(void)
{
    /* 注意：__HAL_HRTIM_GET_ITSTATUS 在本 HAL 版本只查 IER（是否使能），
     * 不查 ISR。要判断"哪一路真的触发"必须用 __HAL_HRTIM_GET_FLAG 读 ISR。*/
    //HAL_GPIO_WritePin(GPIOA,GPIO_PIN_12,GPIO_PIN_SET);
    if (__HAL_HRTIM_GET_FLAG(&hhrtim1, HRTIM_FLAG_FLT1))
    {
        __HAL_HRTIM_CLEAR_FLAG(&hhrtim1, HRTIM_FLAG_FLT1);
        __HAL_HRTIM_DISABLE_IT(&hhrtim1, HRTIM_IT_FLT1);   /* 防中断风暴 */
        g_fault.flt1_cnt++;
        g_fault.last_fault = FAULT_FLT1;
        FAULT_LED_ON(LED1_GPIO_Port, LED1_Pin);            /* COMP2/PA3 */
    }
    if (__HAL_HRTIM_GET_FLAG(&hhrtim1, HRTIM_FLAG_FLT2))
    {
        __HAL_HRTIM_CLEAR_FLAG(&hhrtim1, HRTIM_FLAG_FLT2);
        __HAL_HRTIM_DISABLE_IT(&hhrtim1, HRTIM_IT_FLT2);
        g_fault.flt2_cnt++;
        g_fault.last_fault = FAULT_FLT2;
        FAULT_LED_ON(LED2_GPIO_Port, LED2_Pin);            /* COMP4/PB0 */
    }
    if (__HAL_HRTIM_GET_FLAG(&hhrtim1, HRTIM_FLAG_FLT3))
    {
        __HAL_HRTIM_CLEAR_FLAG(&hhrtim1, HRTIM_FLAG_FLT3);
        __HAL_HRTIM_DISABLE_IT(&hhrtim1, HRTIM_IT_FLT3);
        g_fault.flt3_cnt++;
        g_fault.last_fault = FAULT_FLT3;
        FAULT_LED_ON(LED3_GPIO_Port, LED3_Pin);            /* COMP6/PB11 */
    }

    g_fault.total_cnt++;
    g_fault.last_tick = HAL_GetTick();
    g_fault.tripped   = 1;
}

void Fault_Report_Poll(void)
{
    /* ---- 1) 故障边沿打印：仅在 total_cnt 变化（有新故障进入 ISR）时打印一次 ---- */
    static uint32_t last_total = 0;
    uint32_t total = g_fault.total_cnt;     /* volatile 快照 */
    if (total != last_total)
    {
        last_total = total;

        const char *src;
        switch (g_fault.last_fault)
        {
            case FAULT_FLT1: src = "FLT1  COMP2/PA3  (DAC1_CH2 REF)"; break;
            case FAULT_FLT2: src = "FLT2  COMP4/PB0  (DAC1_CH1 REF)"; break;
            case FAULT_FLT3: src = "FLT3  COMP6/PB11 (DAC4_CH2 REF)"; break;
            default:         src = "NONE";                             break;
        }

        printf("\r\n==== HRTIM FAULT TRIGGER! (OCP/OVP, PWM LOCK) ====\r\n");
        printf("  LAST TRIGGER : %s\r\n", src);
        printf("  TRIGGER TIME: %lu ms\r\n", (unsigned long)g_fault.last_tick);
        printf("  FAULT COUNT : FLT1=%lu  FLT2=%lu  FLT3=%lu  总计=%lu\r\n",
               (unsigned long)g_fault.flt1_cnt,
               (unsigned long)g_fault.flt2_cnt,
               (unsigned long)g_fault.flt3_cnt,
               (unsigned long)g_fault.total_cnt);
        printf("  LOCK FLAG : tripped=%u （FIND OUT PROBRAM THEN  Fault_Rearm() TO RESTART\r\n",
               g_fault.tripped);

        uint32_t per = llc_period;
        if (per)
        {
            printf("  FAULT OCCURRED : period=%lu, fsw=%lu Hz\r\n",
                   (unsigned long)per,
                   (unsigned long)(HRTIM_EQUIV_CLK_HZ / per));
        }
        printf("====================================================\r\n");
    }

    /* ---- 1b) VOUT OVP 边沿打印：ISR 检测到过压时 g_ovp_cnt++，此处打印 ---- */
    {
        static uint32_t last_ovp = 0;
        uint32_t ovp = g_ovp_cnt;
        if (ovp != last_ovp)
        {
            last_ovp = ovp;
            printf("\r\n==== VOUT OVP TRIGGER! (software, VOUT > %lu mV) ====\r\n",
                   (unsigned long)PI_VOUT_OVP_MV);
            printf("  OVP COUNT : %lu\r\n", (unsigned long)ovp);
            printf("  VOUT filt : %u (%u mV)\r\n", g_vout_filt, g_vout_mv);
            printf("====================================================\r\n");
        }
    }

    /* ---- 2) 每秒状态心跳：核对实测开关频率 / 软启动是否到位 ---- */
    static uint32_t last_hb = 0;
    uint32_t now = HAL_GetTick();
    if ((now - last_hb) >= 1000U)
    {
        last_hb = now;
        uint32_t per = llc_period;
        static const char *const st_name[] = {
            "INIT", "WAIT_AUX", "SOFTSTART", "RUN", "FAULT"
        };
        safe_state_t st = g_safe_state;
#if ADC_APP_ENABLE_VOUT
        printf("[STAT] state=%s period=%lu fsw=%lu Hz done=%u tripped=%u flt=%lu ovp=%lu | VAUX raw=%u filt=%u (%u mV) | VOUT raw=%u filt=%u (%u mV) | PI err=%ld P=%ld I=%ld\r\n",
               (st <= SAFE_FAULT) ? st_name[st] : "?",
               (unsigned long)per,
               (unsigned long)(per ? (HRTIM_EQUIV_CLK_HZ / per) : 0),
               softstart_done,
               g_fault.tripped,
               (unsigned long)g_fault.total_cnt,
               (unsigned long)g_ovp_cnt,
               g_vaux_raw, g_vaux_filt, g_vaux_mv,
               g_vout_raw, g_vout_filt, g_vout_mv,
               (long)g_pi.error, (long)g_pi.p_term, (long)g_pi.i_term);
#else
        printf("[STAT] state=%s period=%lu fsw=%lu Hz done=%u tripped=%u flt=%lu ovp=%lu | VAUX raw=%u filt=%u (%u mV) | PI err=%ld P=%ld I=%ld\r\n",
               (st <= SAFE_FAULT) ? st_name[st] : "?",
               (unsigned long)per,
               (unsigned long)(per ? (HRTIM_EQUIV_CLK_HZ / per) : 0),
               softstart_done,
               g_fault.tripped,
               (unsigned long)g_fault.total_cnt,
               (unsigned long)g_ovp_cnt,
               g_vaux_raw, g_vaux_filt, g_vaux_mv,
               (long)g_pi.error, (long)g_pi.p_term, (long)g_pi.i_term);
#endif
    }

    /* ---- 3) 软启动完成后打印一次 Timer A / Timer C 关键寄存器，定位 260k 问题 ----
     * 关注：两路 PER 是否一致、CMP1/CMP4 是否合理、TIMxCR 的 HALF 位、SET1/RST1
     * 输出源、RSTR 计数器复位源。A 正常(130k)、C 翻倍(260k)，对比即可看出差异。*/
    static uint8_t reg_dumped = 0;
    if (softstart_done && !reg_dumped)
    {
        reg_dumped = 1;
        printf("[REGS] MASTER MCR=%08lX  MPER=%lu\r\n",
               (unsigned long)HRTIM1->sMasterRegs.MCR,
               (unsigned long)HRTIM1->sMasterRegs.MPER);
        printf("[REGS] TIMA CR=%08lX PER=%lu CMP1=%lu CMP4=%lu SET1=%08lX RST1=%08lX RSTR=%08lX\r\n",
               (unsigned long)HRTIM1->sTimerxRegs[0].TIMxCR,
               (unsigned long)HRTIM1->sTimerxRegs[0].PERxR,
               (unsigned long)HRTIM1->sTimerxRegs[0].CMP1xR,
               (unsigned long)HRTIM1->sTimerxRegs[0].CMP4xR,
               (unsigned long)HRTIM1->sTimerxRegs[0].SETx1R,
               (unsigned long)HRTIM1->sTimerxRegs[0].RSTx1R,
               (unsigned long)HRTIM1->sTimerxRegs[0].RSTxR);
        printf("[REGS] TIMC CR=%08lX PER=%lu CMP1=%lu CMP4=%lu SET1=%08lX RST1=%08lX RSTR=%08lX\r\n",
               (unsigned long)HRTIM1->sTimerxRegs[2].TIMxCR,
               (unsigned long)HRTIM1->sTimerxRegs[2].PERxR,
               (unsigned long)HRTIM1->sTimerxRegs[2].CMP1xR,
               (unsigned long)HRTIM1->sTimerxRegs[2].CMP4xR,
               (unsigned long)HRTIM1->sTimerxRegs[2].SETx1R,
               (unsigned long)HRTIM1->sTimerxRegs[2].RSTx1R,
               (unsigned long)HRTIM1->sTimerxRegs[2].RSTxR);
    }
}

void Fault_Rearm(void)
{
    /* 仅在确认故障源（过流/过压）已排除后调用！*/

    /* 1. 清 HRTIM 故障标志 */
    HRTIM1->sCommonRegs.ICR = HRTIM_ICR_FLT1C | HRTIM_ICR_FLT2C | HRTIM_ICR_FLT3C;

    /* 2. 重新使能 PWM 输出（fault 触发后输出被强制到 inactive，必须重启输出）*/
    HAL_HRTIM_WaveformOutputStart(&hhrtim1,
        HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
        HRTIM_OUTPUT_TC1 | HRTIM_OUTPUT_TC2);

    /* 3. 清记录、重新武装中断 */
    g_fault.tripped    = 0;
    g_fault.last_fault = FAULT_NONE;
    __HAL_HRTIM_CLEAR_FLAG(&hhrtim1, FLT_FLAG_ALL);
    __HAL_HRTIM_ENABLE_IT(&hhrtim1, FLT_IT_ALL);

    /* 熄灭三路故障灯 */
    FAULT_LED_OFF(LED1_GPIO_Port, LED1_Pin);
    FAULT_LED_OFF(LED2_GPIO_Port, LED2_Pin);
    FAULT_LED_OFF(LED3_GPIO_Port, LED3_Pin);
}
