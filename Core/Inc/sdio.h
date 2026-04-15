#ifndef __SDIO_H__
#define __SDIO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "stm32f4xx_hal_sd.h"

extern SD_HandleTypeDef   hsd;


void MX_SDIO_SD_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* __SDIO_H__ */
