#ifndef VAUX_ADC_H
#define VAUX_ADC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ----------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "tim.h"

/* ============================================================================
 *  VAUX 采样（原 VOUT 采样改造）
 *  --------------------------------------------------------------------------
 *  硬件：原采样 VOUT 的那一路 ADC（ADC1 规则组 Rank1 = IN2 = PA1）已飞线改接到
 *        辅助电源 24V 轨的分压点（分压电阻、引脚不变，仅语义改变）。
 *  同一分压点还并接到 COMP2(PA3) 的 IN+，用作 21V 硬件欠压闸（见 safe_sm.h）。
 *  采样换算（分压比 / VREF / 满量程）全部沿用原 VOUT 工程现状，不重算。
 * ==========================================================================*/

/* ADC 参考电压(mV) 与 12bit 满量程（沿用工程现状）*/
#define VAUX_ADC_VREF_MV    3300U
#define VAUX_ADC_FULLSCALE  4095U

/* 板上分压比：真实 VAUX = 引脚电压 ×(R上+R下)/R下。
 * 沿用原 VOUT 分压：NUM=10, DEN=1 → 引脚 = VAUX/10。
 * 24V→2.4V、23V→2.3V、22V→2.2V、21V→2.1V、5V→0.5V，全部落在 0~3.3V 内。*/
#define VAUX_DIV_NUM        10U   /* (R上 + R下) */
#define VAUX_DIV_DEN        1U    /* (R下) */

/* ---- 标定校正（沿用原 VOUT 标定方法，默认不校正）----
 * VAUX(mV) = 理论值 × VAUX_CAL_GAIN + VAUX_CAL_OFFSET_MV */
#define VAUX_CAL_GAIN        1.0f
#define VAUX_CAL_OFFSET_MV   100.0f

/* 真实电压(mV) -> 引脚(mV) -> ADC/DAC 码（12bit, 3.3V 基准）。
 * 给 safe_sm.h 的门限常量复用，保证「门限的伏特值」与「ADC/DAC 码」同源换算。*/
#define VAUX_PIN_MV(vaux_mv)  (((uint32_t)(vaux_mv) * VAUX_DIV_DEN) / VAUX_DIV_NUM)
#define VAUX_MV_TO_CODE(vaux_mv) \
            ((uint16_t)((VAUX_PIN_MV(vaux_mv) * VAUX_ADC_FULLSCALE) / VAUX_ADC_VREF_MV))

/* 一阶 IIR(EWMA) 轻滤波移位：alpha = 1/2^N。
 * N=3 → alpha=1/8，10kHz 采样下 τ≈8×100µs=0.8ms (<1ms)，抑制单点尖峰误触发。*/
#define VAUX_FILT_SHIFT      3U

/* 最近一次采样结果（TIM3 中断里写，主循环/串口里读，故 volatile）*/
extern volatile uint16_t g_vaux_raw;    /* ADC 原始值 0..4095（未滤波）*/
extern volatile uint16_t g_vaux_filt;   /* EWMA 滤波后的 ADC 码（门限比较用，整数快路径）*/
extern volatile uint16_t g_vaux_mv;     /* 换算后的辅源电压 (mV)，仅供串口显示 */

/* 上电调用一次：ADC1 单端自校准 + 启动 TIM3 周期中断(10kHz)驱动采样。
 * 须在 MX_ADC1_Init / MX_TIM3_Init 之后调用。*/
void VAUX_ADC_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* VAUX_ADC_H */
