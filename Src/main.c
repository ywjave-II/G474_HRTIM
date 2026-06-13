/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "comp.h"
#include "dac.h"
#include "hrtim.h"
#include "iwdg.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "arm_math.h"
#include "io_retarget.h"
#include <iso646.h>
#include <stdio.h>
#include "freq_skip.h"
#include "fault_log.h"
#include "vaux_adc.h"
#include "safe_sm.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

const uint16_t sine_table[64] = {
     931,1022,1112,1201,1287,1370,1448,1522,
    1589,1650,1705,1752,1791,1822,1844,1857,
    1861,1857,1844,1822,1791,1752,1705,1650,
    1589,1522,1448,1370,1287,1201,1112,1022,
     931, 840, 750, 661, 575, 492, 414, 340,
     273, 212, 157, 110,  71,  40,  18,   5,
       0,   5,  18,  40,  71, 110, 157, 212,
     273, 340, 414, 492, 575, 661, 750, 840
};






/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_HRTIM1_Init();
  MX_DAC1_Init();
  MX_ADC1_Init();
  MX_COMP2_Init();
  MX_COMP4_Init();
  MX_COMP6_Init();
  MX_DAC2_Init();
  MX_DAC4_Init();
  MX_ADC2_Init();
  MX_USART1_UART_Init();
  MX_TIM3_Init();
  MX_IWDG_Init();
  /* USER CODE BEGIN 2 */

  /* 调试时冻结 IWDG：核心 halt(断点/单步)时停狗，避免调试中被看门狗复位。
   * 不影响正常运行（仅在调试器暂停内核时生效）。*/
  __HAL_DBGMCU_FREEZE_IWDG();

  /* ---- INIT：复位后第一件事，强制封波 + DIS 失能，杜绝重启窗口出现 PWM ---- */
  SafeSM_Init();                 /* HRTIM 输出 inactive + DIS=SET(失能) + 状态归 WAIT_AUX */
  SafeSM_ConfigBrownout();       /* MCU 自身 PVD 欠压预警（BOR 见 safe_sm.c 顶部，为 option byte）*/

  /* 启动辅助电源(VAUX：原 VOUT/PA1 已飞线改接 24V 轨)采样：ADC1 自校准 + TIM3 10kHz */
  VAUX_ADC_Init();

  /* 关闭 stdout 缓冲：裸机 newlib 默认全缓冲，\n 不 flush，会导致"串口没输出"。*/
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("\r\n[BOOT] G474_HRTIM start, USART1 115200 8N1\r\n");
  printf("[BOOT] safe-reentry armed: wait VAUX>=%umV stable before PWM\r\n",
         (unsigned)VAUX_REARM_MV);

//   // 禁用Fault，清除上电误触发
HAL_HRTIM_FaultModeCtl(&hhrtim1, HRTIM_FAULT_1, HRTIM_FAULTMODECTL_DISABLED);
HAL_HRTIM_FaultModeCtl(&hhrtim1, HRTIM_FAULT_2, HRTIM_FAULTMODECTL_DISABLED);
HAL_HRTIM_FaultModeCtl(&hhrtim1, HRTIM_FAULT_3, HRTIM_FAULTMODECTL_DISABLED);
HRTIM1->sCommonRegs.ICR = HRTIM_ICR_FLT1C | HRTIM_ICR_FLT2C|HRTIM_ICR_FLT3C;




HAL_COMP_Start(&hcomp2);
HAL_DAC_Start(&hdac1,DAC1_CHANNEL_2 );
/* COMP2(PA3, IN+=VAUX 分压) 阈值改为 VAUX 欠压闸 21V(≈2606 码)。配合 HRTIM Fault1
 * 「低有效 + latched」(CubeMX 配置)：COMP 输出低=VAUX<21V=故障，硬件纳秒级封死 PWM。*/
HAL_DAC_SetValue(&hdac1, DAC1_CHANNEL_2, DAC_ALIGN_12B_R, VAUX_HW_TRIP_DAC);
HAL_COMP_Start(&hcomp4);
HAL_DAC_Start(&hdac1,DAC1_CHANNEL_1 );
HAL_DAC_SetValue(&hdac1, DAC1_CHANNEL_1, DAC_ALIGN_12B_R, 3500);
HAL_COMP_Start(&hcomp6);
HAL_DAC_Start(&hdac4,DAC1_CHANNEL_2 );
HAL_DAC_SetValue(&hdac4, DAC1_CHANNEL_2, DAC_ALIGN_12B_R, 3500);


// // 等待dac上拉稳定
HAL_Delay(1);


// // 清除延迟期间可能产生的误触发
HRTIM1->sCommonRegs.ICR = HRTIM_ICR_FLT1C | HRTIM_ICR_FLT2C|HRTIM_ICR_FLT3C;

// // 重新使能Fault 1
HAL_HRTIM_FaultModeCtl(&hhrtim1, HRTIM_FAULT_1, HRTIM_FAULTMODECTL_ENABLED);
HAL_HRTIM_FaultModeCtl(&hhrtim1, HRTIM_FAULT_2, HRTIM_FAULTMODECTL_ENABLED);
HAL_HRTIM_FaultModeCtl(&hhrtim1, HRTIM_FAULT_3, HRTIM_FAULTMODECTL_ENABLED);


/* 使能 HRTIM 故障中断（FLT1=VAUX 欠压, FLT2/3=OCP/OVP），触发时软件记录并锁死。
 * 注意：此处【不】启动 PWM、【不】使能 DIS、【不】调 LLC_SoftStart_Init —— 这些全部推迟到
 * 安全状态机 WAIT_AUX→SOFTSTART（确认 VAUX≥23V 稳定后）才执行，杜绝重启窗口误开通。
 * 若上电时 VAUX<21V，FLT1(低有效) 会立即锁存，属预期；SM 启动时再统一清除。
 * PWM 启动序列(WaveformOutputStart + CountStart(MASTER|TIMER_A) + 使能 DIS + LLC_SoftStart_Init)
 * 现由 safe_sm.c 的 SafeSM_Poll() 在 WAIT_AUX→SOFTSTART 转移里完成。*/
Fault_IRQ_Enable();



  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    HAL_IWDG_Refresh(&hiwdg);  /* 喂狗：只要主循环还在转就喂；卡死(busy-wait/ISR风暴/UART楔死)→停喂→~2s 后复位回 INIT 安全态 */
    SafeSM_Poll();          /* 安全状态机：WAIT_AUX/SOFTSTART/RUN/FAULT 转移 + PVD 轮询 */
    Fault_Report_Poll();    /* 串口诊断：故障详情 + [STAT] 心跳 + [REGS] 一次性寄存器 dump */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV3;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
