#include "mbedtls/build_info.h"
#include "mbedtls/entropy.h"

#if defined(MBEDTLS_PLATFORM_MS_TIME_ALT)
#include "mbedtls/platform_time.h"
#endif

#include "stm32f4xx_hal.h"
#include <string.h>

extern RNG_HandleTypeDef hrng;

/*
 * Аппаратный источник энтропии на базе RNG STM32F4.
 * Вызывается через mbedtls_entropy_func() -> mbedtls_hardware_poll().
 *
 * Требует:
 *  - включённый HAL_RNG_MODULE_ENABLED в stm32f4xx_hal_conf.h
 *  - MX_RNG_Init() вызван до tlsOneTimeInit()
 */
#if defined(MBEDTLS_ENTROPY_HARDWARE_ALT)
int mbedtls_hardware_poll(void *data,
                          unsigned char *output,
                          size_t len,
                          size_t *olen)
{
    (void)data;

    if (output == NULL || olen == NULL) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

    *olen = 0;

    for (size_t i = 0; i < len; i += sizeof(uint32_t)) {
        uint32_t rnd = 0;

        /* Небольшой retry-loop на случай временного BUSY RNG */
        HAL_StatusTypeDef st;
        uint32_t t0 = HAL_GetTick();
        do {
            st = HAL_RNG_GenerateRandomNumber(&hrng, &rnd);
        } while (st == HAL_BUSY && (HAL_GetTick() - t0) < 50U);

        if (st != HAL_OK) {
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }

        size_t copy = (len - i < sizeof(uint32_t)) ? (len - i)
                                                   : sizeof(uint32_t);
        memcpy(output + i, &rnd, copy);
    }

    *olen = len;
    return 0;
}
#endif /* MBEDTLS_ENTROPY_HARDWARE_ALT */

/*
 * Реализация mbedtls_ms_time() для STM32.
 * Нужна, если определён MBEDTLS_PLATFORM_MS_TIME_ALT
 * (platform_util.c без неё не соберётся).
 */
#if defined(MBEDTLS_PLATFORM_MS_TIME_ALT)
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)HAL_GetTick();
}
#endif /* MBEDTLS_PLATFORM_MS_TIME_ALT */
