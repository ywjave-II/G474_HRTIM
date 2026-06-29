#ifndef BURST_MODE_H
#define BURST_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ----------------------------------------------------------*/
#include "main.h"

/* ============================================================================
 *  Burst Mode — 空载/轻载间歇停波防过压
 *  --------------------------------------------------------------------------
 *  问题：空载时 LLC 增益极高，即使 PI 将频率推到 300kHz（最低增益），
 *        VOUT 仍持续上升超过目标值，PI 已经饱和无力降压。
 *
 *  进入（双通道）：
 *    Ch1（电流基）：IOUT < 100mA 持续 5ms → 典型轻载/空载
 *    Ch2（电压基）：VOUT > 24.5V 且 IOUT < 400mA → 立即响应（无去抖）
 *                   中轻载电压爬升兜底，电流确认防大载误切
 *  退出：
 *    IOUT > 400mA 持续 5ms → 回到 RUN（PI 闭环）
 *
 *  行为：
 *    Burst ON:  VOUT < ON 门限  → PWM 使能，固定 300kHz（最低增益）
 *    Burst OFF: VOUT > OFF 门限 → PWM 失能，靠负载/漏电流自然放电
 *
 *  校准：阈值宏为默认值，运行时可通过串口指令修改全局变量
 *        g_burst_* 进行标定，无需重新编译。
 * ==========================================================================*/

/* ---- 默认阈值（待标定；运行时可通过串口修改全局变量微调）---- */

/* IOUT 进入/退出门限 (mA)，带迟滞防抖动 */
#define BURST_IOUT_ENTER_MA_DEF    100     /* IOUT < 此值 → 进入 Burst */
#define BURST_IOUT_EXIT_MA_DEF     400     /* IOUT > 此值 → 退出 Burst（迟滞 100mA）*/

/* VOUT 滞回门限 (mV)：Burst ON/OFF 切换点 */
#define BURST_VOUT_ON_MV_DEF       23800   /* VOUT < 23.8V → 使能 PWM（burst on）*/
#define BURST_VOUT_OFF_MV_DEF      24200   /* VOUT > 24.2V → 失能 PWM（burst off）*/

/* Burst 期间固定频率：160kHz（中频增益，减小能量传输）*/
#define BURST_PERIOD              34000U  /* 160kHz = 5440MHz / 34000 */

/* IOUT 低于/高于门限需持续保持的时长 (ms)，防噪声误触发 */
#define BURST_DEBOUNCE_MS          5U      /* 5ms 去抖 */

/* Burst Step 中 VOUT 比较的抗噪去抖次数（@1kHz 采样率）*/
#define BURST_VOUT_DEBOUNCE_CNT    2U      /* 2 次连续同向比较才切换 */

/* ---- 运行时标定变量（可通过串口指令修改）---- */
extern volatile int32_t  g_burst_iout_enter_ma;   /* IOUT 进入门限 (mA) */
extern volatile int32_t  g_burst_iout_exit_ma;    /* IOUT 退出门限 (mA) */
extern volatile int32_t  g_burst_vout_on_mv;      /* VOUT 滞回下门限 (mV) */
extern volatile int32_t  g_burst_vout_off_mv;     /* VOUT 滞回上门限 (mV) */

/* ---- 诊断计数器 ---- */
extern volatile uint32_t g_burst_on_count;        /* Burst ON 累计次数 */
extern volatile uint32_t g_burst_off_count;       /* Burst OFF 累计次数 */

/* ---- API ---- */

/* 进入 Burst 状态时调用一次（主循环 SafeSM_Poll）：
 * - mV 门限换算为 ADC 码门限
 * - 设固定 Burst 频率
 * - 使能 PWM 输出（初态为 ON）
 * - 复位去抖计数器 */
void BURST_Init(void);

/* ISR 快速进入（TIM3 ISR，10kHz）：VOUT 已超 off 门限时立即 ODISR 关 PWM +
 * 设 BURST 周期 + 复位去抖，不经主循环。与 BURST_Init 的区别：关输出而非开输出。*/
void BURST_FastEnter(void);

/* TIM3 ISR 中调用（1kHz，PI_CTRL_Step 内部）：
 * 读 DMA buffer 原始 ADC 码 → 整数滞回比较 → 使能/失能 PWM 输出 */
void BURST_Step(void);

/* 主循环 SafeSM_Poll RUN 分支调用，双通道进入：
 *   Ch1: IOUT < enter 门限 持续 5ms → 返回 1（去抖）
 *   Ch2: VOUT > off 门限 且 IOUT < exit 门限 → 立即返回 1（无去抖，电压超限需秒级响应）*/
int BURST_ShouldEnter(void);

/* 主循环 SafeSM_Poll BURST 分支调用：IOUT 高于退出门限持续 5ms → 返回 1 */
int BURST_ShouldExit(void);

#ifdef __cplusplus
}
#endif

#endif /* BURST_MODE_H */
