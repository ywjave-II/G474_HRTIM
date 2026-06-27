#ifndef ADC_APP_H
#define ADC_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ----------------------------------------------------------*/
#include "main.h"
#include "adc.h"     /* hadc1, hadc2 */
#include "tim.h"     /* htim3 */

/* ============================================================================
 *  统一 ADC 采样模块 — 所有 ADC 采样通道集中管理
 *  --------------------------------------------------------------------------
 *  使用方式：把需要的通道开关设为 1，不用的保持 0。
 *  条件编译保证了未启用的通道不占代码空间、不占 CPU 时间。
 *
 *  当前通道：
 *    VAUX   — 辅助电源 24V 轨,  ADC1/IN2 /PA1  (10kHz TIM3 驱动，喂 SafeSM)
 *    VOUT   — LLC 输出电压,      ADC2/IN12/PB2  (已启用)
 *    I_CYCLE — 谐振腔电流,       ADC2/IN12/PB2  (硬件已配，待启用)
 *    IOUT    — 输出电流,          ADC2/IN5 /PC4  (DMA 序列 Rank1，与 VOUT 同扫描)
 *
 *  ADC1（VAUX）仍用软件轮询；ADC2（VOUT+IOUT）已改为 TIM3 TRGO 硬件触发 +
 *  DMA 扫描序列搬运，ISR 内直接读 DMA buffer，不再软件轮询。
 * ==========================================================================*/

/* ---- 通道开关：1=启用, 0=停用（对应代码不会被编译）--------------------- */
#define ADC_APP_ENABLE_VAUX    1   /* 辅助电源 24V 轨 — 安全链路核心，勿关 */
#define ADC_APP_ENABLE_VOUT    1   /* LLC 输出电压 — 待硬件接线后改为 1 */
#define ADC_APP_ENABLE_ICYCLE  0   /* 谐振腔电流   — 闭环阶段启用 */
#define ADC_APP_ENABLE_IOUT     1   /* 输出电流     — DMA 序列 Rank1；ADC2/IN5/PC4 */

/* ---- 通用参数（所有通道共享）------------------------------------------ */
#define ADC_APP_VREF_MV       3300U  /* ADC 参考电压(mV) */
#define ADC_APP_FULLSCALE     4095U  /* 12bit 满量程 */
#define ADC_APP_FILT_SHIFT    3U     /* EWMA α=1/2^N, N=3→1/8, τ≈0.8ms@10kHz */

/* ============================================================================
 *  通道参数 — 每个通道独立 #if 块，互不依赖
 * ==========================================================================*/

/* ---- VAUX：辅助电源 24V 轨 (ADC1/IN2/PA1) — 沿用原 vaux_adc.h 参数 ------ */
#if ADC_APP_ENABLE_VAUX
#define ADC_VAUX_HANDLE       hadc1
#define ADC_VAUX_CHANNEL      ADC_CHANNEL_2
#define ADC_VAUX_DIV_NUM      10U
#define ADC_VAUX_DIV_DEN      1U
#define ADC_VAUX_CAL_GAIN     1.0f
#define ADC_VAUX_CAL_OFFSET   100.0f    /* 2026-06-14: 临时试探值，待实测标定 */
#endif

/* ---- VOUT：LLC 输出电压 (ADC2/IN12/PB2) --------------------------------- */
#if ADC_APP_ENABLE_VOUT
#define ADC_VOUT_HANDLE       hadc2
#define ADC_VOUT_CHANNEL      ADC_CHANNEL_12
#define ADC_VOUT_DIV_NUM      11U       /* TODO: 按实际分压电阻填 */
#define ADC_VOUT_DIV_DEN      1U
#define ADC_VOUT_CAL_GAIN     1.0f
#define ADC_VOUT_CAL_OFFSET   0.0f  
#endif

/* ---- I_CYCLE：谐振腔电流 (ADC2/IN12/PB2) — 与 VOUT 互斥 ------------------------------- */
#if ADC_APP_ENABLE_ICYCLE
#define ADC_ICYCLE_HANDLE     hadc2
#define ADC_ICYCLE_CHANNEL    ADC_CHANNEL_12
/* 电流通道通常不需要分压比——采样电阻 + 运放增益，换算为 mA/A。
 * 格式：I(mA) = raw × SCALE_NUM / SCALE_DEN + OFFSET_MA */
#define ADC_ICYCLE_SCALE_NUM  1U        /* TODO: 按采样电阻+运放填 */
#define ADC_ICYCLE_SCALE_DEN  1U
#define ADC_ICYCLE_OFFSET_MA  0
#endif

/* ---- IOUT：输出电流 (ADC2/IN5/PC4) -----------------------
 * 硬件链路：Rshunt=20mΩ, G=50, Voffset=0
 * I(mA) = raw × 3300/4095 / (0.02×50) = raw × 220 / 273    */
#if ADC_APP_ENABLE_IOUT
#define ADC_IOUT_HANDLE        hadc2
#define ADC_IOUT_CHANNEL       ADC_CHANNEL_5
#define ADC_IOUT_SCALE_NUM     3300U   /* VREF(mV) */
#define ADC_IOUT_SCALE_DEN     4095U   /* 12bit FS */
#define ADC_IOUT_OFFSET_MA     0
#endif

/* ============================================================================
 *  换算宏 — 真实电压(mV) ↔ 引脚电压(mV) ↔ ADC/DAC 码
 *  电压宏可直接用于 DAC 阈值（DAC 与 ADC 同为 12bit/3.3V 基准）。
 * ==========================================================================*/
/* 真实电压 → 引脚电压（分压后）*/
#define ADC_PIN_MV(v_mv, div_num, div_den)  (((uint32_t)(v_mv) * (div_den)) / (div_num))

/* 真实电压 → ADC/DAC 码 */
#define ADC_MV_TO_CODE(v_mv, div_num, div_den) \
    ((uint16_t)((ADC_PIN_MV(v_mv, div_num, div_den) * ADC_APP_FULLSCALE) / ADC_APP_VREF_MV))

/* ---- VAUX 专用别名（向后兼容 safe_sm.h 的现有引用）--------------------- */
#if ADC_APP_ENABLE_VAUX
#define VAUX_PIN_MV(vaux_mv)      ADC_PIN_MV(vaux_mv, ADC_VAUX_DIV_NUM, ADC_VAUX_DIV_DEN)
#define VAUX_MV_TO_CODE(vaux_mv)  ADC_MV_TO_CODE(vaux_mv, ADC_VAUX_DIV_NUM, ADC_VAUX_DIV_DEN)
#endif

/* ---- VOUT 专用别名（方便保护阈值引用）----------------------------------- */
#if ADC_APP_ENABLE_VOUT
#define VOUT_PIN_MV(vout_mv)      ADC_PIN_MV(vout_mv, ADC_VOUT_DIV_NUM, ADC_VOUT_DIV_DEN)
#define VOUT_MV_TO_CODE(vout_mv)  ADC_MV_TO_CODE(vout_mv, ADC_VOUT_DIV_NUM, ADC_VOUT_DIV_DEN)
#endif

/* ============================================================================
 *  全局采样数据（TIM3 ISR 里写，主循环/串口里读，故 volatile）
 * ==========================================================================*/
#if ADC_APP_ENABLE_VAUX
extern volatile uint16_t g_vaux_raw;     /* ADC 原始值 0..4095（未滤波）*/
extern volatile uint16_t g_vaux_filt;    /* EWMA 滤波后的 ADC 码（门限比较用，整数快路径）*/
extern volatile uint16_t g_vaux_mv;      /* 换算后的辅源电压 (mV)，仅供串口显示 */
#endif

#if ADC_APP_ENABLE_VOUT
extern volatile uint16_t g_vout_raw;
extern volatile uint16_t g_vout_filt;
extern volatile uint16_t g_vout_mv;
#endif

#if ADC_APP_ENABLE_ICYCLE
extern volatile uint16_t g_icycle_raw;
extern volatile uint16_t g_icycle_filt;
extern volatile int32_t  g_icycle_ma;    /* 电流值 (mA)，有符号 */
#endif

#if ADC_APP_ENABLE_IOUT
extern volatile uint16_t g_iout_raw;
extern volatile uint16_t g_iout_filt;
extern volatile int32_t  g_iout_ma;
#endif

/* ============================================================================
 *  API
 * ==========================================================================*/

/* 上电调用一次（在 MX_ADC1_Init / MX_ADC2_Init / MX_TIM3_Init 之后）：
 * - 校准启用的 ADC（单端自校准）
 * - 启动 TIM3 10kHz 周期中断驱动所有采样 */
void ADC_APP_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* ADC_APP_H */
