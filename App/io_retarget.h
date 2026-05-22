#ifndef __IO_RETARGET_H
#define __IO_RETARGET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

int __io_putchar(int ch);
int __io_getchar(void);

#ifdef __cplusplus
}
#endif

#endif /* __IO_RETARGET_H */
