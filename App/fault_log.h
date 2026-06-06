#ifndef FAULT_LOG_H
#define FAULT_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ----------------------------------------------------------*/
#include "main.h"
#include "hrtim.h"

/* Exported types ----------------------------------------------------*/

/* 故障通道编号（与 HRTIM Fault 线 / 比较器一一对应）*/
typedef enum {
    FAULT_NONE = 0,
    FAULT_FLT1 = 1,   /* HRTIM Fault1 <- COMP2 (PA3)  vs DAC1_CH2 */
    FAULT_FLT2 = 2,   /* HRTIM Fault2 <- COMP4 (PB0)  vs DAC1_CH1 */
    FAULT_FLT3 = 3,   /* HRTIM Fault3 <- COMP6 (PB11) vs DAC4_CH2 */
} fault_id_t;

/* 故障记录（所有字段在中断里写，主循环/串口里读，故 volatile）*/
typedef struct {
    uint32_t   flt1_cnt;    /* 各路累计触发次数 */
    uint32_t   flt2_cnt;
    uint32_t   flt3_cnt;
    uint32_t   total_cnt;   /* 进入 FLT 中断的总次数 */
    fault_id_t last_fault;  /* 最近一次触发的通道 */
    uint32_t   last_tick;   /* 最近一次触发时刻 (HAL_GetTick, ms) */
    uint8_t    tripped;     /* 1 = 保护已锁死，PWM 已被硬件封锁 */
} fault_record_t;

/* Exported variables ------------------------------------------------*/
extern volatile fault_record_t g_fault;

/* Exported functions prototypes ------------------------------------*/

/* 使能 FLT1/2/3 中断 + NVIC。
 * 须在 PWM/Fault 已使能、ICR 已清之后调用（main.c USER CODE 2 末尾）。*/
void Fault_IRQ_Enable(void);

/* 在 HRTIM1_FLT_IRQHandler 内调用：
 * 判别是哪一路触发 -> 记录(次数/时刻/锁死标志) -> 点亮故障灯
 * -> 关闭本路 FLT 中断（防止比较器电平保持高时反复进中断造成风暴）。*/
void Fault_OnIRQ(void);

/* 确认故障源已排除后调用（例如串口指令触发），重新武装保护并恢复 PWM 输出。
 * 注意：对 LLC 过流/过压，通常应人工确认安全后再调用，不要自动恢复。*/
void Fault_Rearm(void);

/* 在主循环 while(1) 里轮询调用（非中断上下文，可安全 printf）：
 *   1) 检测到新故障（total_cnt 变化）时，经 USART3 打印故障详情；
 *   2) 每秒打印一次状态心跳（当前开关周期/频率、软启动状态），便于核对
 *      实测 PWM 频率。所有打印都在主循环完成，绝不在 FLT 中断里 printf。*/
void Fault_Report_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* FAULT_LOG_H */
