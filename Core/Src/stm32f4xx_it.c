/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "stm32f4xx_it.h"

/* USER CODE BEGIN Includes */
#include "uart_ringbuf.hpp"
/* USER CODE END Includes */

/* External variables --------------------------------------------------------*/
extern RTC_HandleTypeDef hrtc;
extern SD_HandleTypeDef  hsd;
extern TIM_HandleTypeDef htim6;

/* USER CODE BEGIN EV */
/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/

void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */
  /* USER CODE END NonMaskableInt_IRQn 0 */
  while (1) {}
}

void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  /* USER CODE END HardFault_IRQn 0 */
  while (1) {}
}

void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1) {}
}

void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  /* USER CODE END BusFault_IRQn 0 */
  while (1) {}
}

void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  /* USER CODE END UsageFault_IRQn 0 */
  while (1) {}
}

void SVC_Handler(void)      {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void)   {}

void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */
  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
  /* USER CODE BEGIN SysTick_IRQn 1 */
  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/******************************************************************************/

void RTC_WKUP_IRQHandler(void)
{
  /* USER CODE BEGIN RTC_WKUP_IRQn 0 */
  /* USER CODE END RTC_WKUP_IRQn 0 */
  HAL_RTCEx_WakeUpTimerIRQHandler(&hrtc);
  /* USER CODE BEGIN RTC_WKUP_IRQn 1 */
  /* USER CODE END RTC_WKUP_IRQn 1 */
}

void SDIO_IRQHandler(void)
{
  /* USER CODE BEGIN SDIO_IRQn 0 */
  /* USER CODE END SDIO_IRQn 0 */
  HAL_SD_IRQHandler(&hsd);
  /* USER CODE BEGIN SDIO_IRQn 1 */
  /* USER CODE END SDIO_IRQn 1 */
}

void TIM6_DAC_IRQHandler(void)
{
  /* USER CODE BEGIN TIM6_DAC_IRQn 0 */
  /* USER CODE END TIM6_DAC_IRQn 0 */
  HAL_TIM_IRQHandler(&htim6);
  /* USER CODE BEGIN TIM6_DAC_IRQn 1 */
  /* USER CODE END TIM6_DAC_IRQn 1 */
}

/* USER CODE BEGIN 1 */
/**
 * @brief USART2 IRQ — Air780E RX приёмник.
 *        Каждый принятый байт кладётся в кольцевой буфер g_air780_rxbuf.
 *        Драйвер air780e.cpp читает из буфера в polling-режиме.
 *        Используем C-обёртку air780_rxbuf_push() т.к. файл компилируется как C.
 */
void USART2_IRQHandler(void)
{
    uint32_t sr = USART2->SR;   /* Читаем SR первым — обязательно для STM32F4 */

    if (sr & USART_SR_RXNE)
    {
        /* Штатный приём: чтение DR сбрасывает RXNE */
        uint8_t ch = (uint8_t)(USART2->DR & 0xFFU);
        air780_rxbuf_push(ch);
        return;
    }

    if (sr & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE))
    {
        /* Ошибочные флаги сбрасываются ТОЛЬКО последовательным чтением SR -> DR.
         * Если не прочитать DR — IRQ будет вызываться бесконечно (stall ISR).
         * NE/FE/PE возникают при старте модема (TX линия ещё не установилась).
         * ORE — байт при этом всё же есть в DR, сохраняем его. */
        uint8_t ch = (uint8_t)(USART2->DR & 0xFFU);
        if (sr & USART_SR_ORE)
        {
            air780_rxbuf_push(ch);
        }
        /* NE / FE / PE при старте — мусорный байт, игнорируем */
    }
}
/* USER CODE END 1 */
