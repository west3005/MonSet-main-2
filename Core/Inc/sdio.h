#ifndef __SDIO_H__
#define __SDIO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "stm32f4xx_hal_sd.h"
#ifdef __cplusplus
  // bool is built-in in C++
#else
# include <stdbool.h>   /* bool for C translation units (sd_diskio.c etc.) */
#endif

extern SD_HandleTypeDef   hsd;


bool MX_SDIO_SD_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* __SDIO_H__ */
