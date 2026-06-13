#include "io_retarget.h"
#include "usart.h"

/* printf 单字符 TX 超时(ms)：115200 下 1 字符≈87µs，10ms 余量极充足。
 * 用有限超时替代 HAL_MAX_DELAY：UART 万一卡住也只阻塞 10ms 后返回，
 * 不会让主循环(SafeSM_Poll/Fault_Report_Poll)永久僵死。*/
#define IO_TX_TIMEOUT_MS  10U

/**
  * @brief  Retargets the C library printf to USART1.
  * @param  ch: Character to output.
  * @retval Character written.
  */
int __io_putchar(int ch)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, IO_TX_TIMEOUT_MS);
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
