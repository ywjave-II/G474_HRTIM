/* ============================================================================
 * 【已停用 / 保留备用】2026-06-12：原 VOUT(PA1) 采样路已飞线改接 24V 轨 → VAUX；
 * 2026-06-17：VOUT 重新启用，改接 ADC2/IN12/PB2，逻辑已迁移至 App/adc_app.c。
 *
 * 下面全部代码（含 #include）用 #if 0 关闭，原因：
 *   1) HAL_TIM_PeriodElapsedCallback 是 HAL 的 __weak 单例，adc_app.c 已实现；
 *   2) VOUT_ADC_Init 已被 ADC_APP_Init 取代；
 *   3) 全局变量 g_vout_raw/g_vout_mv 已在 adc_app.c 定义。
 *
 * 头文件 vout_adc.h 保留不动（参数定义，供参考）。
 * ==========================================================================*/
#if 0

#include "vout_adc.h"

volatile uint16_t g_vout_raw = 0;
volatile uint16_t g_vout_mv  = 0;

void VOUT_ADC_Init(void)
{
    HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
    HAL_TIM_Base_Start_IT(&htim3);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM3)
    {
        return;
    }

    HAL_ADC_Start(&hadc1);

    uint32_t guard = 4000U;
    while (!__HAL_ADC_GET_FLAG(&hadc1, ADC_FLAG_EOC) && (--guard != 0U))
    {
    }

    if (guard != 0U)
    {
        uint16_t raw = (uint16_t)HAL_ADC_GetValue(&hadc1);
        g_vout_raw = raw;

        uint32_t pin_mv = ((uint32_t)raw * VOUT_ADC_VREF_MV) / VOUT_ADC_FULLSCALE;
        float vout = (float)pin_mv * (float)VOUT_DIV_NUM / (float)VOUT_DIV_DEN;
        vout = vout * VOUT_CAL_GAIN + VOUT_CAL_OFFSET_MV;
        if (vout < 0.0f) { vout = 0.0f; }
        g_vout_mv = (uint16_t)(vout + 0.5f);
    }

    HAL_ADC_Stop(&hadc1);
}

#endif /* 已停用：见文件顶部说明 */
