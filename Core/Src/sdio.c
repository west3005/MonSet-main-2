/**
 * @file sdio.c
 * @brief SDIO для SD-карты через HAL_SD (без DMA, без IRQ).
 *
 * ClockDiv=118 → SDIO_CK = 48 MHz / (118+2) = 400 кГц — идентификация.
 * sd_diskio.c поднимет ClockDiv до 0 (24 МГц) после HAL_SD_Init().
 *
 * ВАЖНО: SDIO_HARDWARE_FLOW_CONTROL_ENABLE запрещён на STM32F4 —
 * errata ES0182 §2.7.1: при ENABLE возникают ложные DCRCFAIL при записи.
 *
 * NOTE: В stm32f4xx_hal_msp.c был незамёрженный git-конфликт, который
 * принудительно выставлял HardwareFlowControl = ENABLE внутри MspInit.
 * Этот конфликт должен быть разрешён — строку с ENABLE удалить.
 * Данный файл явно переназначает DISABLE после HAL_SD_Init() как
 * дополнительная защита.
 */
#include "sdio.h"
#include "stm32f4xx_hal_sd.h"
#include "main.h"

SD_HandleTypeDef hsd;

bool MX_SDIO_SD_Init(void)
{
    hsd.Instance             = SDIO;
    hsd.Init.ClockEdge       = SDIO_CLOCK_EDGE_RISING;
    hsd.Init.ClockBypass     = SDIO_CLOCK_BYPASS_DISABLE;
    hsd.Init.ClockPowerSave  = SDIO_CLOCK_POWER_SAVE_DISABLE;
    hsd.Init.BusWide         = SDIO_BUS_WIDE_1B;

    /*
     * ОБЯЗАТЕЛЬНО DISABLE на STM32F4 — errata ES0182 §2.7.1 (HWFC_EN).
     * При ENABLE генерируются ложные DCRCFAIL/DTIMEOUT при записи.
     *
     * ВНИМАНИЕ: в stm32f4xx_hal_msp.c::HAL_SD_MspInit() есть
     * незамёрженный git-конфликт, который перезаписывает это значение
     * на ENABLE. Необходимо исправить HAL_SD_MspInit() — см. ниже.
     */
    hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;

    /*
     * ClockDiv=118: SDIO_CK = SDIOCLK(48 MHz) / (118+2) = 400 кГц.
     * sd_diskio.c поднимет ClockDiv=0 (24 МГц) после инициализации.
     */
    hsd.Init.ClockDiv = 118U;

    /* Attempt 1: straight init */
    if (HAL_SD_Init(&hsd) != HAL_OK) {
        /* Errata ES0182 §2.7.1 — force HWFC=DISABLE and retry once */
        hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
        SDIO->CLKCR &= ~SDIO_CLKCR_HWFC_EN;
        if (HAL_SD_Init(&hsd) != HAL_OK) {
            return false;  /* No card / wiring issue — caller sets g_sd_disabled */
        }
    }

    /* Lock bus width to 1-bit (D1..D3 not routed on this board) */
    if (HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_1B) != HAL_OK) {
        return false;
    }
    return true;
}
