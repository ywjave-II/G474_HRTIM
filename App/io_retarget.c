#include "io_retarget.h"
#include "usart.h"

/**
  * @brief  Retargets the C library printf to USART1.
  * @param  ch: Character to output.
  * @retval Character written.
  */
int __io_putchar(int ch)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
  return ch;
}

/**
  * @brief  Retargets the C library scanf to USART1.
  * @retval Character read.
  */
int __io_getchar(void)
{
  uint8_t ch;
  HAL_UART_Receive(&huart1, &ch, 1, HAL_MAX_DELAY);
  return ch;
}
