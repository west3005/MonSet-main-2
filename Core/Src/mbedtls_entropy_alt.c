#include "mbedtls/build_info.h"
#include "mbedtls/entropy.h"
#include "stm32f4xx_hal.h"
#include <string.h>

#if defined(MBEDTLS_PLATFORM_MS_TIME_ALT)
#include "mbedtls/platform_time.h"
#endif

extern RNG_HandleTypeDef hrng;

/*
 * Аппаратный источник энтропии на базе RNG STM32F4.
 * Вызывается из mbedtls_entropy_func (ctr_drbg_seed).
 * Требует инициализированного hrng (MX_RNG_Init вызван до этого).
 */
#if defined(MBEDTLS_ENTROPY_HARDWARE_ALT)
int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen)
{
    (void)data;

    if (output == NULL || olen == NULL) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

    *olen = 0;

    for (size_t i = 0; i < len; i += sizeof(uint32_t)) {
        uint32_t rnd = 0;

        /* Таймаут 100 мс на случай временной занятости RNG */
        uint32_t t0 = HAL_GetTick();
        HAL_StatusTypeDef st;
        do {
            st = HAL_RNG_GenerateRandomNumber(&hrng, &rnd);
        } while (st != HAL_OK && (HAL_GetTick() - t0) < 100U);

        if (st != HAL_OK) {
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }

        size_t copy = (len - i < sizeof(uint32_t)) ? (len - i) : sizeof(uint32_t);
        memcpy(output + i, &rnd, copy);
    }

    *olen = len;
    return 0;
}
#endif /* MBEDTLS_ENTROPY_HARDWARE_ALT */

/*
 * Реализация mbedtls_ms_time() для STM32.
 * Нужна если определён MBEDTLS_PLATFORM_MS_TIME_ALT.
 * После отключения TLS1.3 используется только для DTLS timeout
 * (который тоже отключён), но оставляем на случай линкера.
 */
#if defined(MBEDTLS_PLATFORM_MS_TIME_ALT)
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)HAL_GetTick();
}
#endif /* MBEDTLS_PLATFORM_MS_TIME_ALT */
