/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    fatfs.h
  * @brief   Header for fatfs applications
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __FATFS_H
#define __FATFS_H

#ifdef __cplusplus
 extern "C" {
#endif

#include "ff.h"
#include "ff_gen_drv.h"
#include "sd_diskio.h"  /* defines SD_Driver as external */  // ← ИСПРАВЛЕНО

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern uint8_t retSD;    /* Return value for SD */
extern char SDPath[4];   /* SD logical drive path */
extern FATFS SDFatFS;    /* File system object for SD logical drive */
extern FIL SDFile;       /* File object for SD */

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_FATFS_Init(void);

/* USER CODE BEGIN Prototypes */

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __FATFS_H */
