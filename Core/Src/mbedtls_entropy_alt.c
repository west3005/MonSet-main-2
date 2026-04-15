#include "mbedtls/entropy.h"
#include "mbedtls/platform_time.h"
#include "stm32f4xx_hal.h"
#include <string.h>

extern RNG_HandleTypeDef hrng;

/*
 * Аппаратный источник энтропии на базе RNG STM32F4.
 * Требует:
 *  - включённый HAL_RNG_MODULE_ENABLED в stm32f4xx_hal_conf.h
 *  - инициализированный hrng в main.cpp
 */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;

    if (output == NULL || olen == NULL) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

    *olen = 0;

    for (size_t i = 0; i < len; i += sizeof(uint32_t)) {
        uint32_t rnd = 0;

        if (HAL_RNG_GenerateRandomNumber(&hrng, &rnd) != HAL_OK) {
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }

        size_t copy = (len - i < sizeof(uint32_t)) ? (len - i) : sizeof(uint32_t);
        memcpy(output + i, &rnd, copy);
    }

    *olen = len;
    return 0;
}

/* У тебя включено MBEDTLS_PLATFORM_MS_TIME_ALT */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)HAL_GetTick();
}
