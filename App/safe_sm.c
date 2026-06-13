#include "safe_sm.h"
#include "freq_skip.h"    /* LLC_SoftStart_Init / softstart_done：复用扫频软启动 */
#include "fault_log.h"    /* g_fault：硬件 FLT 触发记录，用于同步状态机 */

/* ============================================================================
 *  BOR（掉电复位）—— 代码自动配置，无需任何外部工具：
 *  G4 的 BOR 门限在 FLASH option byte(FLASH_OPTR.BOR_LEV)，上电默认 Level 0(~1.7V) 太低。
 *  SafeSM_EnsureBOR() 在首启时把它提到 Level 4(VDD↓≈2.8V 复位)，保证 MCU VDD 跌到不可靠区
 *  前就硬复位（复位即重新 INIT 硬封波，比 PVD 更彻底）。
 *  做法：先读当前档，若已是目标档就直接跳过；否则编程 option byte 并 OB_Launch（触发一次
 *  系统复位）。复位后再进来时已是目标档 -> 跳过 -> 正常启动。故只在「刚烧录后的第一次上电」
 *  多一次自动重启，之后无感。你只需照常烧录固件，BOR 自动生效。
 *  ⚠️ 首次烧录后会自动复位一次，调试器(DAPLink/OpenOCD)可能瞬断重连一次，属正常。
 * ==========================================================================*/
#define SAFE_BOR_LEVEL   OB_BOR_LEVEL_4   /* VDD↓≈2.8V 触发掉电复位 */

#define SAFE_OUT_ALL  (HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | \
                       HRTIM_OUTPUT_TC1 | HRTIM_OUTPUT_TC2)
#define SAFE_FLT_IT_ALL    (HRTIM_IT_FLT1   | HRTIM_IT_FLT2   | HRTIM_IT_FLT3)
#define SAFE_FLT_FLAG_ALL  (HRTIM_FLAG_FLT1 | HRTIM_FLAG_FLT2 | HRTIM_FLAG_FLT3)

volatile safe_state_t g_safe_state = SAFE_INIT;

/* WAIT_AUX 中 VAUX 持续高于 REARM 的计时起点 */
static uint8_t  aux_ok_timing = 0;
static uint32_t aux_ok_t0     = 0;

void SafeSM_EnterFault(void)
{
    /* 幂等：HRTIM 输出强制 inactive（断输出）+ DIS 失能。可从 ISR/主循环安全调用。
     * 注：硬件 FLT 触发时输出已被硬件封死，这里再 Stop 一次确保软件路也封死且不依赖硬件。*/
    HAL_HRTIM_WaveformOutputStop(&hhrtim1, SAFE_OUT_ALL);
    HALF_BRIDGE_DISABLE();
    g_safe_state = SAFE_FAULT;
}

/* 首启一次把 BOR 提到目标档（已是目标档则跳过）。编程后 OB_Launch 触发系统复位。*/
static void SafeSM_EnsureBOR(void)
{
    FLASH_OBProgramInitTypeDef ob = {0};
    HAL_FLASHEx_OBGetConfig(&ob);

    if ((ob.USERConfig & FLASH_OPTR_BOR_LEV) == SAFE_BOR_LEVEL)
    {
        return;   /* 已是目标档：跳过，避免每次上电重写 option byte */
    }

    HAL_FLASH_Unlock();
    HAL_FLASH_OB_Unlock();

    ob.OptionType = OPTIONBYTE_USER;
    ob.USERType   = OB_USER_BOR_LEV;
    ob.USERConfig = SAFE_BOR_LEVEL;
    if (HAL_FLASHEx_OBProgram(&ob) == HAL_OK)
    {
        HAL_FLASH_OB_Launch();   /* 重载 option byte -> 系统复位（仅首启一次，之后跳过）*/
    }

    HAL_FLASH_OB_Lock();
    HAL_FLASH_Lock();
}

void SafeSM_Init(void)
{
    /* INIT：复位后、启动任何 PWM 之前的第一件事 —— 默认就「不输出」。*/
    aux_ok_timing     = 0;

    HAL_HRTIM_WaveformOutputStop(&hhrtim1, SAFE_OUT_ALL);   /* 输出强制 inactive */
    HALF_BRIDGE_DISABLE();                                  /* DIS 失能（PA12 直驱拉高 = DIS 高 = 失能）*/

    /* 先封波再处理 BOR：BOR 若需编程会在此触发系统复位（首启一次），复位后重新进来跳过。*/
    SafeSM_EnsureBOR();

    g_safe_state = SAFE_WAIT_AUX;   /* 进入等辅源，保持封波，由 SafeSM_Poll 推进 */
}

void SafeSM_ConfigBrownout(void)
{
    /* PVD：监测 MCU 自身 VDD(3.3V 轨)，预警级。不挂中断（避免与 CubeMX 重新生成的
     * PVD_PVM_IRQHandler 冲突，regen-safe），主循环 SafeSM_Poll() 轮询 PVDO 标志。*/
    PWR_PVDTypeDef sConfigPVD = {0};
    sConfigPVD.PVDLevel = SAFE_PVD_LEVEL;        /* ≈2.9V */
    sConfigPVD.Mode     = PWR_PVD_MODE_NORMAL;   /* 仅置 PVDO 标志，不产生 EXTI 中断 */
    HAL_PWR_ConfigPVD(&sConfigPVD);
    HAL_PWR_EnablePVD();
}

void SafeSM_OnSample(uint16_t vaux_code)
{
    /* 软件立即封波：仅在已输出(SOFTSTART/RUN)时跌破 22V 才动作。
     * WAIT_AUX/INIT/FAULT 本就封波，不重复触发。整数短路径。*/
    if (vaux_code < VAUX_SW_TRIP_CODE)
    {
        if (g_safe_state == SAFE_SOFTSTART || g_safe_state == SAFE_RUN)
        {
            SafeSM_EnterFault();
        }
    }
}

/* 清 HRTIM 锁存故障 + 重新武装 FLT 记录中断 + 熄灯。仅在重启互锁满足后调用。*/
static void SafeSM_ClearLatchedFault(void)
{
    HRTIM1->sCommonRegs.ICR = HRTIM_ICR_FLT1C | HRTIM_ICR_FLT2C | HRTIM_ICR_FLT3C;
    __HAL_HRTIM_CLEAR_FLAG(&hhrtim1, SAFE_FLT_FLAG_ALL);

    g_fault.tripped    = 0;
    g_fault.last_fault = FAULT_NONE;

    /* Fault_OnIRQ 触发后会关闭对应路 FLT 中断防风暴，这里统一重开 */
    __HAL_HRTIM_ENABLE_IT(&hhrtim1, SAFE_FLT_IT_ALL);

    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);   /* 开漏，SET=熄灭 */
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_SET);
}

void SafeSM_Poll(void)
{
    /* PVD 二次校验：MCU VDD 跌破 ~2.9V 时也优雅封波（BOR 会在更低处硬复位兜底）*/
    if (__HAL_PWR_GET_FLAG(PWR_FLAG_PVDO) != RESET)
    {
        if (g_safe_state == SAFE_SOFTSTART || g_safe_state == SAFE_RUN)
        {
            SafeSM_EnterFault();
        }
    }

    switch (g_safe_state)
    {
        case SAFE_WAIT_AUX:
            /* 等辅源回升到 REARM(23V) 且稳定保持 VAUX_STABLE_MS，再放行启动 */
            if (g_vaux_filt >= VAUX_REARM_CODE)
            {
                if (!aux_ok_timing)
                {
                    aux_ok_timing = 1;
                    aux_ok_t0     = HAL_GetTick();
                }
                else if ((HAL_GetTick() - aux_ok_t0) >= VAUX_STABLE_MS)
                {
                    aux_ok_timing = 0;

                    /* 自检通过 -> 启动：清硬件锁存故障 -> 使能 DIS -> 重走完整软启动 */
                    SafeSM_ClearLatchedFault();
                    HALF_BRIDGE_ENABLE();
                    LLC_SoftStart_Init();     /* 复用：重启输出 + CountStart + MREP IT + 周期归位 */
                    g_safe_state = SAFE_SOFTSTART;
                }
            }
            else
            {
                aux_ok_timing = 0;            /* 跌回阈值下，稳定计时作废 */
            }
            break;

        case SAFE_SOFTSTART:
            if (g_fault.tripped)              /* 软启动期间硬件 OCP/OVP/UVP 触发 */
            {
                SafeSM_EnterFault();
            }
            else if (softstart_done)          /* 扫频到位 -> 定频运行 */
            {
                g_safe_state = SAFE_RUN;
            }
            break;

        case SAFE_RUN:
            if (g_fault.tripped)              /* 运行期间任何硬件 FLT 触发 -> 封波 */
            {
                SafeSM_EnterFault();
            }
            break;

        case SAFE_FAULT:
            /* 重启条件：VAUX 回升到 REARM(23V) 即回 WAIT_AUX，由其完成「稳定50ms自检
             * → 清 HRTIM 锁存故障 → 重走完整 SOFTSTART」。真正断透时 MCU 与 UCC21520 VCCI
             * 共用同一供电链(24V→DCDC→5V→LDO→3.3V)，必随之掉电，下次为冷上电从 INIT 天然
             * 安全重入，软件不再模拟「断透确认」。仅在 22~23V 抖动够不到 REARM → 停在 FAULT，
             * 靠 HRTIM 锁存故障保持封波，不自动重启。*/
            if (g_vaux_filt >= VAUX_REARM_CODE)
            {
                g_safe_state = SAFE_WAIT_AUX;
            }
            break;

        case SAFE_INIT:
        default:
            /* 正常不应停在 INIT；兜底拉回安全态 */
            SafeSM_EnterFault();
            break;
    }
}
