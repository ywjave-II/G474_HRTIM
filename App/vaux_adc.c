#include "vaux_adc.h"
#include "safe_sm.h"     /* SafeSM_OnSample()：把滤波后的采样喂给安全状态机 */

volatile uint16_t g_vaux_raw  = 0;
volatile uint16_t g_vaux_filt = 0;
volatile uint16_t g_vaux_mv   = 0;

void VAUX_ADC_Init(void)
{
    /* ADC 单端自校准：必须在第一次启动转换之前做一次，提升精度 */
    HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);

    /* 启动 TIM3 周期中断(10kHz)。每次更新事件 -> HAL_TIM_PeriodElapsedCallback */
    HAL_TIM_Base_Start_IT(&htim3);
}

/* TIM3 周期到(10kHz)：软件触发一次 ADC1 规则转换并读取 VAUX。
 * 用自旋等 EOC（不依赖 HAL_GetTick，可安全用于中断上下文；ADC 异常时也不会死锁）。
 * 关键：这是「立即封波」软件路的采样节拍——读到 VAUX，滤波，再交给状态机判 22V 欠压。
 * 临界路径全整数（EWMA + 码比较），mV 换算的浮点只用于串口显示，不在封波判决里。
 * 注意：这是 HAL 的 __weak 回调，全工程只能有一处实现。*/
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
        g_vaux_raw = raw;

        /* 一阶 EWMA：acc 保存 (filt << SHIFT) 的累加器，filt = acc >> SHIFT。
         * 首样直接预置，避免上电从 0 缓慢爬升期间误判欠压。*/
        static uint32_t acc = 0;
        static uint8_t  primed = 0;
        if (!primed) { acc = (uint32_t)raw << VAUX_FILT_SHIFT; primed = 1; }
        else         { acc += raw - (acc >> VAUX_FILT_SHIFT); }
        uint16_t filt = (uint16_t)(acc >> VAUX_FILT_SHIFT);
        g_vaux_filt = filt;

        /* 喂给安全状态机：22V 软件欠压立即封波。整数、确定性、短路径。*/
        SafeSM_OnSample(filt);

        /* 串口显示用：引脚电压 -> 分压还原 -> 标定。浮点仅此处，非封波路径。*/
        uint32_t pin_mv = ((uint32_t)raw * VAUX_ADC_VREF_MV) / VAUX_ADC_FULLSCALE;
        float vaux = (float)pin_mv * (float)VAUX_DIV_NUM / (float)VAUX_DIV_DEN;
        vaux = vaux * VAUX_CAL_GAIN + VAUX_CAL_OFFSET_MV;
        if (vaux < 0.0f) { vaux = 0.0f; }
        g_vaux_mv = (uint16_t)(vaux + 0.5f);
    }

    HAL_ADC_Stop(&hadc1);
}
