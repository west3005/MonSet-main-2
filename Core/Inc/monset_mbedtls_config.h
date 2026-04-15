#ifndef MONSET_MBEDTLS_CONFIG_H
#define MONSET_MBEDTLS_CONFIG_H

/* Берём “родной” конфиг из пакета mbedTLS… */
#include "mbedtls/mbedtls_config.h"

/* …и накладываем STM32-override’ы. */

/* 1) Исправление вашей текущей ошибки:
   entropy_poll.c падает на не-Unix/не-Windows, если НЕ задано MBEDTLS_NO_PLATFORM_ENTROPY. */
#ifndef MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_NO_PLATFORM_ENTROPY
#endif

/* На embedded обычно нужен свой источник энтропии. */
#ifndef MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#endif

/* 2) Рекомендуется отключить TLS 1.3 на STM32, пока не доведёте конфиг до конца. */
#ifdef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_PROTO_TLS1_3
#endif
#ifdef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#endif
#ifdef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#endif
#ifdef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#endif
#ifdef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#endif
#ifdef MBEDTLS_SSL_EARLY_DATA
#undef MBEDTLS_SSL_EARLY_DATA
#endif

/* 3) На STM32 нет POSIX/WinSock — модуль net_sockets вам не нужен. */
#ifdef MBEDTLS_NET_C
#undef MBEDTLS_NET_C
#endif

/* 4) timing.c в основном для примеров/тестов; если вы его не используете — отключаем. */
#ifdef MBEDTLS_TIMING_C
#undef MBEDTLS_TIMING_C
#endif

/* Дадим свою реализацию mbedtls_ms_time() для STM32 */
#ifndef MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#endif

/* На STM32/arm-none-eabi нет POSIX dirent/opendir(), поэтому FS_IO выключаем.
   Иначе x509_crt.c полезет в <dirent.h> и упадёт. */
#ifdef MBEDTLS_FS_IO
#undef MBEDTLS_FS_IO
#endif

/* PSA ITS file backend требует MBEDTLS_FS_IO (stdio/filesystem).
   На STM32 мы FS_IO отключаем (нет POSIX), поэтому отключаем и этот backend. */
#ifdef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#endif

#ifdef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#endif

#endif /* MONSET_MBEDTLS_CONFIG_H */
