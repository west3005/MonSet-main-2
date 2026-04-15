/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file sd_diskio.c
  * @brief SD Disk I/O driver — polling HAL, no DMA, no IRQ disable.
  *        SDIO_FLAG_DCRCFAIL cleared on error so retries work correctly.
  *
  * NOTE: This is a pure C file. Do NOT include C++ headers (debug_uart.hpp).
  *       Logging uses uart_log_* wrappers declared in debug_uart_c.h.
  ******************************************************************************
  */
/* USER CODE END Header */
#include "ff_gen_drv.h"
#include "sd_diskio.h"
#include "sdio.h"
#include "stm32f4xx_hal_sd.h"
#include "main.h"

/* C-compatible log helpers */
#include "debug_uart_c.h"

/* Timeout: 30 seconds */
#ifndef SD_TIMEOUT
#define SD_TIMEOUT   (30U * 1000U)
#endif

#define SD_DEFAULT_BLOCK_SIZE  512U

static volatile DSTATUS Stat = STA_NOINIT;

/* =========================================================================
 * Forward declarations
 * ========================================================================= */
DSTATUS SD_initialize(BYTE);
DSTATUS SD_status    (BYTE);
DRESULT SD_read      (BYTE, BYTE *, DWORD, UINT);
#if _USE_WRITE == 1
DRESULT SD_write     (BYTE, const BYTE *, DWORD, UINT);
#endif
#if _USE_IOCTL == 1
DRESULT SD_ioctl     (BYTE, BYTE, void *);
#endif

const Diskio_drvTypeDef SD_Driver = {
    SD_initialize,
    SD_status,
    SD_read,
#if _USE_WRITE == 1
    SD_write,
#endif
#if _USE_IOCTL == 1
    SD_ioctl,
#endif
};

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * @brief Clear all SDIO error/status flags so the next operation starts clean.
 */
static inline void SD_ClearFlags(void)
{
    __HAL_SD_CLEAR_FLAG(&hsd,
        SDIO_FLAG_CCRCFAIL | SDIO_FLAG_DCRCFAIL |
        SDIO_FLAG_CTIMEOUT | SDIO_FLAG_DTIMEOUT |
        SDIO_FLAG_TXUNDERR | SDIO_FLAG_RXOVERR  |
        SDIO_FLAG_CMDREND  | SDIO_FLAG_CMDSENT  |
        SDIO_FLAG_DATAEND  | SDIO_FLAG_STBITERR |
        SDIO_FLAG_DBCKEND);
}

/**
 * @brief Poll until card enters TRANSFER state or timeout.
 * @note  Uses HAL_GetTick() — interrupts MUST remain enabled.
 *        Uses subtraction comparison to be overflow-safe.
 */
static HAL_StatusTypeDef SD_WaitCardReady(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout_ms) {
        if (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER) {
            return HAL_OK;
        }
        HAL_Delay(1);
    }
    return HAL_TIMEOUT;
}

static DSTATUS SD_CheckStatus(BYTE lun)
{
    (void)lun;
    Stat = STA_NOINIT;
    if (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER) {
        Stat &= (DSTATUS)~STA_NOINIT;
    }
    return Stat;
}

/* =========================================================================
 * FATFS disk interface
 * ========================================================================= */

DSTATUS SD_initialize(BYTE lun)
{
    (void)lun;
    Stat = STA_NOINIT;

    /* Already running — just check state */
    if (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER) {
        return SD_CheckStatus(lun);
    }

    /* Re-init sequence */
    SD_ClearFlags();
    if (HAL_SD_DeInit(&hsd) != HAL_OK) {
        return Stat;
    }

    /* Low speed for identification phase (ClockDiv set in MX_SDIO_SD_Init) */
    MX_SDIO_SD_Init();
    if (hsd.State == HAL_SD_STATE_ERROR) {
        return Stat;
    }

    if (SD_WaitCardReady(2000U) != HAL_OK) {
        return Stat;
    }

    /*
     * Switch to normal operating speed after init: 24 MHz (ClockDiv=0).
     * SDIO_CK = SDIOCLK(48 MHz) / (0+2) = 24 MHz — максимум для 1-bit шины.
     * HWFC отключён в sdio.c (errata F4 §2.7.1), поэтому ClockDiv=0 безопасен.
     */
    hsd.Init.ClockDiv = 0U;
    MODIFY_REG(SDIO->CLKCR, SDIO_CLKCR_CLKDIV, 0U);

    Stat = SD_CheckStatus(lun);
    uart_log_info("[DISKIO] init: Stat=0x%02X", (unsigned)Stat);
    return Stat;
}

DSTATUS SD_status(BYTE lun)
{
    return SD_CheckStatus(lun);
}

DRESULT SD_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
    HAL_StatusTypeDef hs;
    (void)lun;

    if (Stat & STA_NOINIT)            { return RES_NOTRDY; }
    if ((buff == NULL) || (count == 0U)) { return RES_PARERR; }

    uart_log_info("[DISKIO] read: sec=%lu cnt=%u",
                  (unsigned long)sector, (unsigned)count);

    hs = HAL_SD_ReadBlocks(&hsd, (uint8_t *)buff,
                           (uint32_t)sector, (uint32_t)count, SD_TIMEOUT);
    if (hs != HAL_OK) {
        uart_log_error("[DISKIO] read: HAL err=%d STA=0x%08lX",
                       (int)hs, (unsigned long)SDIO->STA);
        SD_ClearFlags();
        return RES_ERROR;
    }

    if (SD_WaitCardReady(SD_TIMEOUT) != HAL_OK) {
        uart_log_error("[DISKIO] read: WaitReady timeout");
        SD_ClearFlags();
        return RES_ERROR;
    }

    uart_log_info("[DISKIO] read: OK");
    return RES_OK;
}

#if _USE_WRITE == 1
DRESULT SD_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
    HAL_StatusTypeDef hs;
    (void)lun;

    if (Stat & STA_NOINIT)            { return RES_NOTRDY; }
    if ((buff == NULL) || (count == 0U)) { return RES_PARERR; }

    uart_log_info("[DISKIO] write: sec=%lu cnt=%u",
                  (unsigned long)sector, (unsigned)count);

    /* Clear any stale flags and give the card a moment to settle */
    SD_ClearFlags();
    HAL_Delay(1);

    /* No IRQ disable — polling mode requires SysTick to be alive */
    hs = HAL_SD_WriteBlocks(&hsd, (uint8_t *)buff,
                            (uint32_t)sector, (uint32_t)count, SD_TIMEOUT);
    if (hs != HAL_OK) {
        uart_log_error("[DISKIO] write: HAL err=%d STA=0x%08lX",
                       (int)hs, (unsigned long)SDIO->STA);
        SD_ClearFlags();
        return RES_ERROR;
    }

    /*
     * Ожидаем перехода карты в TRANSFER (overflow-safe: использует вычитание).
     * HAL_GetTick() + SD_TIMEOUT — классический overflow-баг на embedded,
     * SD_WaitCardReady использует (GetTick() - t0) < timeout_ms.
     */
    if (SD_WaitCardReady(SD_TIMEOUT) != HAL_OK) {
        uart_log_error("[DISKIO] write: WaitReady timeout");
        SD_ClearFlags();
        return RES_ERROR;
    }

    uart_log_info("[DISKIO] write: OK");
    return RES_OK;
}
#endif /* _USE_WRITE */

#if _USE_IOCTL == 1
DRESULT SD_ioctl(BYTE lun, BYTE cmd, void *buff)
{
    (void)lun;
    DRESULT res = RES_ERROR;

    if (Stat & STA_NOINIT) { return RES_NOTRDY; }

    switch (cmd) {
        case CTRL_SYNC: {
            SD_ClearFlags();
            res = (SD_WaitCardReady(SD_TIMEOUT) == HAL_OK) ? RES_OK : RES_ERROR;
            break;
        }
        case GET_SECTOR_COUNT: {
            HAL_SD_CardInfoTypeDef info;
            HAL_SD_GetCardInfo(&hsd, &info);
            *(DWORD *)buff = info.LogBlockNbr;
            res = RES_OK;
            break;
        }
        case GET_SECTOR_SIZE: {
            HAL_SD_CardInfoTypeDef info;
            HAL_SD_GetCardInfo(&hsd, &info);
            *(WORD *)buff = (WORD)info.LogBlockSize;
            res = RES_OK;
            break;
        }
        case GET_BLOCK_SIZE: {
            HAL_SD_CardInfoTypeDef info;
            HAL_SD_GetCardInfo(&hsd, &info);
            *(DWORD *)buff = info.LogBlockSize / SD_DEFAULT_BLOCK_SIZE;
            res = RES_OK;
            break;
        }
        default:
            res = RES_PARERR;
            break;
    }
    return res;
}
#endif /* _USE_IOCTL */
