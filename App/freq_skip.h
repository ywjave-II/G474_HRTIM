#ifndef FREQ_SKIP_H
#define FREQ_SKIP_H

#ifdef __cplusplus
extern "C" {
#endif


/* Includes ----------------------------------------------------------*/
// #define LLC_FREQ_START_PER    21760U   // 250kHz 起始（高频=低增益）
// #define LLC_FREQ_TARGET_PER   41846U   // 130kHz 目标（谐振频率）
#define LLC_FREQ_START_PER    18133U   // 300kHz 起始（高频=低增益）（开环测试，压低谐振腔电流）
// #define LLC_FREQ_TARGET_PER   39100U   // 139.5kHz 目标（开环测试限制FSW>FR频率，防止进入容性区）
#define LLC_FREQ_TARGET_PER   40300U   // 135kHz 目标（开环测试限制FSW=FR频率，测试谐振点增益）
#define LLC_SOFTSTART_STEP    10U      // 每次 REP 中断增加的 tick 数
                                    // 步进越小越平滑，越大越快到位
#define LLC_SKIP_COUNT        10U  //每10次中断修改一次周期？

/* CMP4 关断点偏移量（tick）— Timer C 的 CMP4 = period - CMP4_OFFSET。
   正常运行时 period > CMP4_OFFSET（最小周期 18133 >> 342）。*/
#define PI_CMP4_OFFSET  342U

#include "main.h"
#include "hrtim.h"
/* Exported types ----------------------------------------------------*/
extern  volatile uint32_t llc_period;
extern  volatile uint8_t  softstart_done;
extern  volatile uint32_t llc_period_frzee;
/* Exported constants ------------------------------------------------*/


/* Exported macro ----------------------------------------------------*/


/* Exported functions prototypes ------------------------------------*/

void LLC_SoftStart_Init(void);
void LLC_SoftStart_Step(void);

/* 统一的 HRTIM 周期寄存器写入（供 PI 闭环 + 软启动共用）。
 * 写入顺序：先 TimerA/C PERxR（预装载），最后 Master MPER（有效寄存器）。
 * 包含 CMP1(180°移相) / CMP4(关断点) 跟随更新 + CMP4 下溢保护。
 * 注意：TimerA CMP1 不写（InterleavedMode=DUAL 由硬件自动 PER/2）。*/
void HRTIM_SetLLCPeriod(uint32_t period);


#ifdef __cplusplus
}
#endif

#endif /* FREQ_SKIP_H */