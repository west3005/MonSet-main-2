/**
 * ================================================================
 * @file power_manager.cpp
 * @brief Реализация Stop Mode с восстановлением тактирования
 *        и полной реинициализацией используемой периферии.
 * ================================================================
 */

#include "power_manager.hpp"
#include "debug_uart.hpp"
#include "main.h"

extern "C" {
void SystemClock_Config(void);

void MX_GPIO_Init(void);
void MX_USART1_UART_Init(void);
void MX_USART2_UART_Init(void);
void MX_USART3_UART_Init(void);
void MX_USART6_UART_Init(void);
void MX_I2C1_Init(void);
void MX_TIM6_Init(void);
void MX_RNG_Init(void);
void MX_SDIO_SD_Init(void);
void MX_FATFS_Init(void);
void MX_SPI1_Init(void);
}

extern bool g_sd_disabled;

/* ========= Локальная проверка тактирования после STOP ========= */

static bool checkClocksAfterStop()
{
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_HSERDY) == RESET) {
        DBG.error("CLK: HSE not ready after STOP");
        return false;
    }

    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PLLRDY) == RESET) {
        DBG.error("CLK: PLL not ready after STOP");
        return false;
    }

    return true;
}

/* ========= Конструктор ========= */

PowerManager::PowerManager(RTC_HandleTypeDef* hrtc, SdBackup& backup)
    : m_hrtc(hrtc)
    , m_backup(backup)
{
}

/* ========= Вход в Stop Mode ========= */

void PowerManager::enterStopMode(uint32_t sec)
{
    /* 1. Деинит SD перед сном */
    m_backup.deinit();

    /* 2. RTC Wakeup Timer: сначала выключаем, затем настраиваем */
    HAL_RTCEx_DeactivateWakeUpTimer(m_hrtc);

    if (HAL_RTCEx_SetWakeUpTimer_IT(
            m_hrtc,
            (sec > 0U) ? (sec - 1U) : 0U,
            RTC_WAKEUPCLOCK_CK_SPRE_16BITS) != HAL_OK) {
        Error_Handler();
    }

    /* 3. Небольшая пауза, чтобы UART успел вывести лог */
    HAL_Delay(50);

    /* 4. Останавливаем SysTick, чтобы не будил MCU в STOP */
    HAL_SuspendTick();

    /* 5. Очищаем флаг пробуждения */
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);

    /* 6. Входим в STOP MODE */
    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
    /* ===== МК спит ===== */

    /* 7. После пробуждения — восстановить системное тактирование */
    SystemClock_Config();

    /* 7.1. Если HSE/PLL не поднялись — fallback на HSI */
    if (!checkClocksAfterStop()) {
        RCC_ClkInitTypeDef clkinit{};
        uint32_t flashLatency = 0;

        __HAL_RCC_SYSCLK_CONFIG(RCC_SYSCLKSOURCE_HSI);

        clkinit.ClockType      = RCC_CLOCKTYPE_SYSCLK |
                                 RCC_CLOCKTYPE_HCLK   |
                                 RCC_CLOCKTYPE_PCLK1  |
                                 RCC_CLOCKTYPE_PCLK2;
        clkinit.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
        clkinit.AHBCLKDivider  = RCC_SYSCLK_DIV1;
        clkinit.APB1CLKDivider = RCC_HCLK_DIV1;
        clkinit.APB2CLKDivider = RCC_HCLK_DIV1;
        flashLatency           = FLASH_LATENCY_0;

        if (HAL_RCC_ClockConfig(&clkinit, flashLatency) != HAL_OK) {
            Error_Handler();
        }
    }

    /* 8. Возобновить SysTick */
    HAL_ResumeTick();

    /* 9. Отключить WakeUp Timer */
    HAL_RTCEx_DeactivateWakeUpTimer(m_hrtc);

    /*
     * 10. Полная реинициализация периферии, которая реально используется
     *     приложением после пробуждения.
     *
     * Порядок близок к main():
     * GPIO -> debug UART -> I2C/UARTs -> RNG -> SPI -> SDIO/FATFS -> TIM6
     */

    MX_GPIO_Init();

    MX_USART1_UART_Init();
    MX_USART6_UART_Init();
    DBG.info("PM: UART1/6 reinit after STOP");

    MX_I2C1_Init();
    DBG.info("PM: I2C1 reinit after STOP");

    MX_USART2_UART_Init();
    DBG.info("PM: UART2 reinit after STOP");

    MX_USART3_UART_Init();
    DBG.info("PM: UART3 reinit after STOP");

    MX_RNG_Init();
    DBG.info("PM: RNG reinit after STOP");

    MX_SPI1_Init();
    DBG.info("PM: SPI1 reinit after STOP");

    if (!g_sd_disabled) {
        MX_SDIO_SD_Init();
        MX_FATFS_Init();
        DBG.info("PM: SDIO/FatFs reinit after STOP");
    } else {
        DBG.warn("PM: SD disabled, skip SDIO/FatFs reinit");
    }

    MX_TIM6_Init();
    DBG.info("PM: TIM6 reinit after STOP");

    /* 11. Вернуть SD backup в рабочее состояние */
    if (!g_sd_disabled) {
        (void)m_backup.init();
    }
}
