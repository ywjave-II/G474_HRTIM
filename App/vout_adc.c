#include "vout_adc.h"

volatile uint16_t g_vout_raw = 0;
volatile uint16_t g_vout_mv  = 0;

void VOUT_ADC_Init(void)
{
    /* ADC 单端自校准：必须在第一次启动转换之前做一次，提升精度 */
    HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);

    /* 启动 TIM3 周期中断(10kHz)。每次更新事件 -> HAL_TIM_PeriodElapsedCallback */
    HAL_TIM_Base_Start_IT(&htim3);
}

/* TIM3 周期到(10kHz)：软件触发一次 ADC1 规则转换并读取 VOUT。
 * 用自旋等 EOC（不依赖 HAL_GetTick，可安全用于中断上下文；ADC 异常时也不会死锁）。
 * 注意：这是 HAL 的 __weak 回调，全工程只能有一处实现；若以后还有别的定时器
 * 也用周期中断，需在这里按 htim->Instance 分支处理。*/
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM3)
    {
        return;
    }

    HAL_ADC_Start(&hadc1);

    /* 转换时间 ≈ (247.5+12.5)/42.5MHz ≈ 6µs；自旋上限给足余量后兜底退出 */
    uint32_t guard = 4000U;
    while (!__HAL_ADC_GET_FLAG(&hadc1, ADC_FLAG_EOC) && (--guard != 0U))
    {
    }

    if (guard != 0U)
    {
        uint16_t raw = (uint16_t)HAL_ADC_GetValue(&hadc1);   /* 读 DR 自动清 EOC */
        g_vout_raw = raw;

        /* 引脚电压(mV) = raw × VREF / 满量程 */
        uint32_t pin_mv = ((uint32_t)raw * VOUT_ADC_VREF_MV) / VOUT_ADC_FULLSCALE;

        /* 按分压比还原 → 标定校正（增益+偏移）。硬件 FPU，单精度，中断里可用 */
        float vout = (float)pin_mv * (float)VOUT_DIV_NUM / (float)VOUT_DIV_DEN;
        vout = vout * VOUT_CAL_GAIN + VOUT_CAL_OFFSET_MV;
        if (vout < 0.0f) { vout = 0.0f; }
        g_vout_mv = (uint16_t)(vout + 0.5f);
    }

    HAL_ADC_Stop(&hadc1);
}
