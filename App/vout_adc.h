#ifndef VOUT_ADC_H
#define VOUT_ADC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ----------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "tim.h"

/* ADC 参考电压(mV) 与 12bit 满量程 */
#define VOUT_ADC_VREF_MV    3300U
#define VOUT_ADC_FULLSCALE  4095U

/* 板上分压比：真实 VOUT = 引脚电压 ×(R上+R下)/R下。
 * 按你的采样分压电阻填写；默认 1:1（即引脚直接等于 VOUT）。
 * 例：VOUT 经 10k/1k 分压 → 引脚=VOUT/11 → NUM=11, DEN=1。*/
#define VOUT_DIV_NUM        10U   /* (R上 + R下) */
#define VOUT_DIV_DEN        1U   /* (R下) */

/* ---- 标定校正（修正分压电阻容差 / ADC 偏差）----
 * 公式：VOUT(mV) = 理论值 × VOUT_CAL_GAIN + VOUT_CAL_OFFSET_MV
 *
 * 标定方法（二选一）：
 *  ① 两点法（最准，同时修增益和偏移）：
 *     调到两个已知输出 V1、V2（万用表测，单位 mV），记下串口对应打印的 M1、M2，
 *     GAIN   = (V1 - V2) / (M1 - M2)
 *     OFFSET = V1 - GAIN × M1
 *  ② 单点增益法（电阻容差为主时够用）：
 *     调到已知 V（mV），记下打印值 M，GAIN = V / M，OFFSET = 0
 * 把算得的值填到下面两个宏。默认不校正（GAIN=1, OFFSET=0）。*/
#define VOUT_CAL_GAIN        1.0f
#define VOUT_CAL_OFFSET_MV   100.0f

/* 最近一次采样结果（在 TIM3 中断里写，主循环/串口里读，故 volatile）*/
extern volatile uint16_t g_vout_raw;   /* ADC 原始值 0..4095 */
extern volatile uint16_t g_vout_mv;    /* 换算后的 VOUT 电压 (mV) */

/* 上电调用一次：ADC1 单端自校准 + 启动 TIM3 周期中断(10kHz)驱动采样。
 * 须在 MX_ADC1_Init / MX_TIM3_Init 之后调用。*/
void VOUT_ADC_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* VOUT_ADC_H */
