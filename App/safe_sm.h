#ifndef SAFE_SM_H
#define SAFE_SM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ----------------------------------------------------------*/
#include "main.h"
#include "hrtim.h"
#include "vaux_adc.h"     /* 换算宏 VAUX_MV_TO_CODE / g_vaux_filt */

/* ============================================================================
 *  辅助电源安全监测 + 安全重入状态机
 *  --------------------------------------------------------------------------
 *  事故背景：连续重启时独立辅源缓慢衰减，MCU 在欠压/不确定态仍输出 PWM，再次上电
 *  瞬间疑似半桥误开通、MOS 击穿。本模块消除这个「重启窗口」。
 *
 *  分层防护（由软到硬、互相兜底）：
 *   1) 软件优雅封波：10kHz ISR 读 VAUX，跌破 22V(VAUX_SW_TRIP) -> enter_fault()
 *   2) 硬件兜底封波：COMP2(PA3,IN+=VAUX分压) vs DAC1_CH2(=21V)，经 HRTIM Fault1
 *      「低有效 + 锁存」，VAUX<21V 时纳秒级硬件封死 PWM，不依赖 ISR 节拍。
 *   3) 器件 UVLO 兜底：UCC21520A(5V版) VDD_OFF=5.4/5.7/6.0V、VCCI_OFF=2.35/2.5/2.65V。
 *      从 22V 降到 6V 这一大段器件 UVLO 够不着，必须靠 1)+2)+DIS 默认失能。
 *   4) MCU 自身：BOR(复位级) + PVD(预警级) 监测 MCU VDD(3.3V 轨)。
 *
 *  方向矛盾的处理：COMP 固有「IN+ > IN- => 输出有效(高)」。要的是「VAUX<阈值=>故障」，
 *  接法不变则方向相反 -> 不动硬件，改用 HRTIM Fault「低有效」配置（COMP 输出低=VAUX低
 *  =故障）。该项与迟滞/blanking/latched 均在 CubeMX 配置（见文件尾 CubeMX 清单）。
 * ==========================================================================*/

/* ---- 门限定义（全部具名常量，集中一处；电压->码用 vaux_adc.h 换算同源推导）---- */
/* VAUX 门限（单位 mV，真实辅源电压）*/
#define VAUX_SW_TRIP_MV   22000U   /* 软件检测：跌破即优雅封波 + 记 log + 进 FAULT */
#define VAUX_HW_TRIP_MV   21000U   /* 硬件 COMP/DAC 闸：比软件低一档，软件优先、硬件兜底 */
#define VAUX_REARM_MV     23000U   /* 迟滞回升点：回到此值并稳定才允许重启 */

/* 由上面伏特值换算出的 ADC 码 / DAC 码（注释为标称值，宏保证与换算同源）*/
#define VAUX_SW_TRIP_CODE   VAUX_MV_TO_CODE(VAUX_SW_TRIP_MV)  /* 22V -> 引脚2.20V -> ADC码≈2730 */
#define VAUX_REARM_CODE     VAUX_MV_TO_CODE(VAUX_REARM_MV)    /* 23V -> 引脚2.30V -> ADC码≈2854 */
#define VAUX_HW_TRIP_DAC    VAUX_MV_TO_CODE(VAUX_HW_TRIP_MV)  /* 21V -> 引脚2.10V -> DAC码≈2606 */

/* VAUX 回升到 REARM 之上需稳定保持的时长(ms)，再放行重启，抗回升毛刺 */
#define VAUX_STABLE_MS      50U

/* MCU PVD 欠压预警档：PWR_PVDLEVEL_6 ≈ 2.9V（最高内部档，最早预警，给 MCU 留最大裕量）。
 * 仅监测 MCU 自身 VDD(3.3V)，与 24V 辅源无关；BOR 在 option byte 配置（见 .c 与 CLAUDE.md）。*/
#define SAFE_PVD_LEVEL      PWR_PVDLEVEL_6

/* ---- DIS（UCC21520A 失能脚 = PA12，PA12 IO 直驱，已拆除 2N7002）----
 * 硬件：PA12 直接接 UCC21520A 的 DIS 脚（非反相，PA12 电平=DIS 电平）。
 * 语义：RESET(低)=DIS 低=使能驱动；SET(高)=DIS 高=失能。上电默认 SET(高)=失能（见 gpio.c）。
 * 实测(0.1V 步进)：VAUX>23.1V→PA12=0V 使能；VAUX<22V→PA12=3.3V 失能，落在 REARM↔SW_TRIP 迟滞带内。
 * ⚠️ 拆 2N7002 后掉电/高阻时 DIS 不再有外部上拉钳失能态；如需 MCU 失电也强制失能，DIS 网络应留对 VCCI 上拉。*/
#define HALF_BRIDGE_ENABLE()   HAL_GPIO_WritePin(DIS_GPIO_Port, DIS_Pin, GPIO_PIN_RESET)
#define HALF_BRIDGE_DISABLE()  HAL_GPIO_WritePin(DIS_GPIO_Port, DIS_Pin, GPIO_PIN_SET)

/* ---- 状态机 ---- */
typedef enum {
    SAFE_INIT = 0,    /* 复位后：强制封波 + DIS 失能（启动任何 PWM 之前）*/
    SAFE_WAIT_AUX,    /* 等辅源 ≥ REARM 且稳定，期间保持封波 */
    SAFE_SOFTSTART,   /* 复用现有 300k->140k 扫频软启动 */
    SAFE_RUN,         /* 扫频结束开环定频运行 */
    SAFE_FAULT        /* 封波 + DIS 失能，停留；VAUX 回升到 REARM 即回 WAIT_AUX 重启 */
} safe_state_t;

extern volatile safe_state_t g_safe_state;

/* 上电初始化：INIT 硬封波(HRTIM 输出 inactive + DIS 失能) + 复位状态，落到 WAIT_AUX。
 * 须在 MX_HRTIM1_Init / MX_GPIO_Init 之后、启动 PWM 之前调用。*/
void SafeSM_Init(void);

/* 配置 MCU 自身欠压保护：PVD(运行期 HAL，不挂中断，主循环轮询 PVDO 标志)。
 * BOR 为 option byte，见 .c 顶部说明，需用 STM32CubeProgrammer 一次性烧写。*/
void SafeSM_ConfigBrownout(void);

/* 10kHz ISR 调用（vaux_adc.c 内）：22V 软件欠压立即封波。
 * 整数、短路径、可重入安全。*/
void SafeSM_OnSample(uint16_t vaux_code);

/* 主循环轮询：处理 WAIT_AUX/SOFTSTART/RUN/FAULT 之间的高层转移、硬件故障同步、
 * PVD 轮询。非中断上下文。*/
void SafeSM_Poll(void);

/* 幂等的「立即进故障」：HRTIM 输出强制 inactive + DIS 失能 + 切 FAULT。
 * 可从任意上下文（ISR/主循环）安全调用。*/
void SafeSM_EnterFault(void);

#ifdef __cplusplus
}
#endif

#endif /* SAFE_SM_H */
