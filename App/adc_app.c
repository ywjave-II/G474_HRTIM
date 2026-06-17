#include "adc_app.h"
#include "safe_sm.h"     /* SafeSM_OnSample()：VAUX 滤波后喂安全状态机 */
#include <stdio.h>       /* 编译器内置，仅用于浮点换算，不用 printf */

/* ============================================================================
 *  全局采样数据定义（每个通道只在此处定义一次；extern 声明在 adc_app.h）
 * ==========================================================================*/
#if ADC_APP_ENABLE_VAUX
volatile uint16_t g_vaux_raw  = 0;
volatile uint16_t g_vaux_filt = 0;
volatile uint16_t g_vaux_mv   = 0;
#endif

#if ADC_APP_ENABLE_VOUT
volatile uint16_t g_vout_raw  = 0;
volatile uint16_t g_vout_filt = 0;
volatile uint16_t g_vout_mv   = 0;
#endif

#if ADC_APP_ENABLE_ICYCLE
volatile uint16_t g_icycle_raw  = 0;
volatile uint16_t g_icycle_filt = 0;
volatile int32_t  g_icycle_ma   = 0;
#endif

#if ADC_APP_ENABLE_IOU
volatile uint16_t g_iou_raw  = 0;
volatile uint16_t g_iou_filt = 0;
volatile int32_t  g_iou_ma   = 0;
#endif

/* ============================================================================
 *  ADC_APP_Init — 上电调用一次
 *  校准所有启用的 ADC + 启动 TIM3 10kHz 周期中断驱动采样。
 *  须在 MX_ADC1_Init / MX_ADC2_Init / MX_TIM3_Init 之后调用。
 * ==========================================================================*/
void ADC_APP_Init(void)
{
#if ADC_APP_ENABLE_VAUX
    HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
#endif

#if ADC_APP_ENABLE_VOUT || ADC_APP_ENABLE_ICYCLE || ADC_APP_ENABLE_IOU
    HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED);
#endif

    /* 启动 TIM3 10kHz 周期中断。每次更新事件 → HAL_TIM_PeriodElapsedCallback */
    HAL_TIM_Base_Start_IT(&htim3);
}

/* ============================================================================
 *  HAL_TIM_PeriodElapsedCallback — 10kHz 采样节拍（全工程唯一实现）
 *
 *  关键设计：
 *   - ADC1 和 ADC2 是独立 SAR ADC，可并行转换。顺序启动各自自旋等 EOC，
 *     总时间 ≈ max(各通道转换时间) 而非累加。
 *   - 每个通道独立 #if 块，块之间无耦合，增删通道只需改开关宏。
 *   - 所有通道（含 VAUX）采样前一律 HAL_ADC_ConfigChannel 重配寄存器：
 *     → 换通道只需改 adc_app.h 的 CHANNEL/HANDLE 宏，无需重生成 CubeMX。
 *     轻量且幂等（仅写几个寄存器），不依赖 MX_ADCx_Init 的初始通道配置。
 *   - 同一 ADC 上多通道时：Stop → 重配通道 → Start → 自旋 → Read → Stop。
 *   - 临界路径全整数（EWMA + 码比较），浮点仅用于串口显示的 mV 换算。
 *   - 此回调是 HAL 的 __weak 单例，全工程只能有一处实现。
 *     vaux_adc.c / vout_adc.c 的旧实现已用 #if 0 停用。
 * ==========================================================================*/
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM3)
    {
        return;
    }

    /* ========================================================================
     *  VAUX — ADC1/IN2/PA1：辅助电源 24V 轨
     *  ★ 安全链路核心：22V 软件欠压在此检测并立即封波
     *  始终重配通道（与其他通道对称）：换通道只需改 adc_app.h，无需重生成 CubeMX。
     * ========================================================================*/
#if ADC_APP_ENABLE_VAUX
    {
        ADC_ChannelConfTypeDef ch = {0};
        ch.Channel      = ADC_VAUX_CHANNEL;
        ch.Rank         = ADC_REGULAR_RANK_1;
        ch.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;
        ch.SingleDiff   = ADC_SINGLE_ENDED;
        HAL_ADC_ConfigChannel(&hadc1, &ch);

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
            if (!primed) { acc = (uint32_t)raw << ADC_APP_FILT_SHIFT; primed = 1; }
            else         { acc += raw - (acc >> ADC_APP_FILT_SHIFT); }
            uint16_t filt = (uint16_t)(acc >> ADC_APP_FILT_SHIFT);
            g_vaux_filt = filt;

            /* 喂给安全状态机：22V 软件欠压立即封波。整数、确定性、短路径。*/
            SafeSM_OnSample(filt);

            /* 串口显示用：引脚电压 → 分压还原 → 标定。浮点仅此处，非封波路径。*/
            uint32_t pin_mv = ((uint32_t)raw * ADC_APP_VREF_MV) / ADC_APP_FULLSCALE;
            float vaux = (float)pin_mv * (float)ADC_VAUX_DIV_NUM / (float)ADC_VAUX_DIV_DEN;
            vaux = vaux * ADC_VAUX_CAL_GAIN + ADC_VAUX_CAL_OFFSET;
            if (vaux < 0.0f) { vaux = 0.0f; }
            g_vaux_mv = (uint16_t)(vaux + 0.5f);
        }

        HAL_ADC_Stop(&hadc1);
    }
#endif /* ADC_APP_ENABLE_VAUX */

    /* ========================================================================
     *  VOUT — ADC2/IN12/PB2：LLC 输出电压
     *  与 VAUX 同一节拍、独立 ADC，并行转换不影响 VAUX 延迟。
     *  启用条件：硬件已飞线 VOUT 分压点 → PB2，且分压比已填入头文件。
     * ========================================================================*/
#if ADC_APP_ENABLE_VOUT
    {
        /* 始终重配通道：轻量且幂等，保证不受 CubeMX 初始通道或其他 ADC2 通道干扰 */
        ADC_ChannelConfTypeDef ch = {0};
        ch.Channel      = ADC_VOUT_CHANNEL;
        ch.Rank         = ADC_REGULAR_RANK_1;
        ch.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;
        ch.SingleDiff   = ADC_SINGLE_ENDED;
        HAL_ADC_ConfigChannel(&hadc2, &ch);

        HAL_ADC_Start(&hadc2);

        uint32_t guard = 4000U;
        while (!__HAL_ADC_GET_FLAG(&hadc2, ADC_FLAG_EOC) && (--guard != 0U))
        {
        }

        if (guard != 0U)
        {
            uint16_t raw = (uint16_t)HAL_ADC_GetValue(&hadc2);
            g_vout_raw = raw;

            static uint32_t acc = 0;
            static uint8_t  primed = 0;
            if (!primed) { acc = (uint32_t)raw << ADC_APP_FILT_SHIFT; primed = 1; }
            else         { acc += raw - (acc >> ADC_APP_FILT_SHIFT); }
            uint16_t filt = (uint16_t)(acc >> ADC_APP_FILT_SHIFT);
            g_vout_filt = filt;

            /* 串口显示用：引脚电压 → 分压还原 → 标定 */
            uint32_t pin_mv = ((uint32_t)raw * ADC_APP_VREF_MV) / ADC_APP_FULLSCALE;
            float vout = (float)pin_mv * (float)ADC_VOUT_DIV_NUM / (float)ADC_VOUT_DIV_DEN;
            vout = vout * ADC_VOUT_CAL_GAIN + ADC_VOUT_CAL_OFFSET;
            if (vout < 0.0f) { vout = 0.0f; }
            g_vout_mv = (uint16_t)(vout + 0.5f);

            /* TODO: 如需 VOUT 过压软件保护，在此调用 SafeSM 或 OVP 回调 */
        }

        HAL_ADC_Stop(&hadc2);
    }
#endif /* ADC_APP_ENABLE_VOUT */

    /* ========================================================================
     *  I_CYCLE — ADC2/IN12/PB2：谐振腔电流
     *  与 VOUT 共用 ADC2，顺序采样（VOUT 已 Stop，此处重配通道后 Start）。
     * ========================================================================*/
#if ADC_APP_ENABLE_ICYCLE
    {
        ADC_ChannelConfTypeDef ch = {0};
        ch.Channel      = ADC_ICYCLE_CHANNEL;
        ch.Rank         = ADC_REGULAR_RANK_1;
        ch.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;
        ch.SingleDiff   = ADC_SINGLE_ENDED;
        HAL_ADC_ConfigChannel(&hadc2, &ch);

        HAL_ADC_Start(&hadc2);

        uint32_t guard = 4000U;
        while (!__HAL_ADC_GET_FLAG(&hadc2, ADC_FLAG_EOC) && (--guard != 0U))
        {
        }

        if (guard != 0U)
        {
            uint16_t raw = (uint16_t)HAL_ADC_GetValue(&hadc2);
            g_icycle_raw = raw;

            static uint32_t acc = 0;
            static uint8_t  primed = 0;
            if (!primed) { acc = (uint32_t)raw << ADC_APP_FILT_SHIFT; primed = 1; }
            else         { acc += raw - (acc >> ADC_APP_FILT_SHIFT); }
            g_icycle_filt = (uint16_t)(acc >> ADC_APP_FILT_SHIFT);

            /* 电流换算：mA = raw × SCALE_NUM / SCALE_DEN + OFFSET */
            g_icycle_ma = (int32_t)(((uint32_t)raw * ADC_ICYCLE_SCALE_NUM) / ADC_ICYCLE_SCALE_DEN)
                        + ADC_ICYCLE_OFFSET_MA;
        }

        HAL_ADC_Stop(&hadc2);
    }
#endif /* ADC_APP_ENABLE_ICYCLE */

    /* ========================================================================
     *  IOU — ADC2/IN5/PC4：输出电流
     * ========================================================================*/
#if ADC_APP_ENABLE_IOU
    {
        ADC_ChannelConfTypeDef ch = {0};
        ch.Channel      = ADC_IOU_CHANNEL;
        ch.Rank         = ADC_REGULAR_RANK_1;
        ch.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;
        ch.SingleDiff   = ADC_SINGLE_ENDED;
        HAL_ADC_ConfigChannel(&hadc2, &ch);

        HAL_ADC_Start(&hadc2);

        uint32_t guard = 4000U;
        while (!__HAL_ADC_GET_FLAG(&hadc2, ADC_FLAG_EOC) && (--guard != 0U))
        {
        }

        if (guard != 0U)
        {
            uint16_t raw = (uint16_t)HAL_ADC_GetValue(&hadc2);
            g_iou_raw = raw;

            static uint32_t acc = 0;
            static uint8_t  primed = 0;
            if (!primed) { acc = (uint32_t)raw << ADC_APP_FILT_SHIFT; primed = 1; }
            else         { acc += raw - (acc >> ADC_APP_FILT_SHIFT); }
            g_iou_filt = (uint16_t)(acc >> ADC_APP_FILT_SHIFT);

            g_iou_ma = (int32_t)(((uint32_t)raw * ADC_IOU_SCALE_NUM) / ADC_IOU_SCALE_DEN)
                     + ADC_IOU_OFFSET_MA;
        }

        HAL_ADC_Stop(&hadc2);
    }
#endif /* ADC_APP_ENABLE_IOU */
}
