/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    dma.c
  * @brief   DMA controller clock enable.
  *          NOTE: SDIO uses polling mode (no DMA).
  *          DMA2_Stream3 and DMA2_Stream6 are NOT used and NOT registered
  *          in NVIC. MX_DMA_Init() only enables the DMA2 peripheral clock
  *          in case other peripherals need it in future.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "dma.h"

void MX_DMA_Init(void)
{
  /* DMA2 peripheral clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /*
   * No DMA streams configured for SDIO.
   * SDIO operates in polling mode:
   *   HAL_SD_ReadBlocks()  / HAL_SD_WriteBlocks() with timeout.
   * SysTick interrupts MUST remain enabled during SD operations
   * so that HAL_GetTick() works correctly inside the HAL.
   */
}
