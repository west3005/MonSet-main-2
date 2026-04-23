#ifndef MONSET_MBEDTLS_CONFIG_H
#define MONSET_MBEDTLS_CONFIG_H

/* Загружаем базовый конфиг */
#include "mbedtls/mbedtls_config.h"

/* ================================================================
 * СТРАТЕГИЯ:
 *   - TLS 1.3 отключён (оставляем только TLS 1.2)
 *   - DTLS отключён (не нужен для HTTPS)
 *   - LMS отключён (не нужен, но требует PSA_CRYPTO_C)
 *   - PSA Crypto ОСТАЁТСЯ (нужен для ECDHE и нормальной работы 3.x)
 *   - Память и таймеры адаптированы под STM32F4 без файловой системы
 * ================================================================ */

/* ── Отключаем TLS 1.3 и его фичи ─────────────────────────────── */
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
#ifdef MBEDTLS_SSL_RECORD_SIZE_LIMIT
#undef MBEDTLS_SSL_RECORD_SIZE_LIMIT
#endif

/* ── LMS требует PSA + SHA256 PSA_WANT — отключаем ───────────── */
#ifdef MBEDTLS_LMS_C
#undef MBEDTLS_LMS_C
#endif
#ifdef MBEDTLS_LMS_PRIVATE
#undef MBEDTLS_LMS_PRIVATE
#endif

/* ── Отключаем DTLS (нам не нужен UDP/DTLS) ───────────────────── */
#ifdef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_PROTO_DTLS
#endif
#ifdef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#endif
#ifdef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#endif
#ifdef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#endif
#ifdef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#endif
#ifdef MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT
#endif
#ifdef MBEDTLS_SSL_DTLS_SRTP
#undef MBEDTLS_SSL_DTLS_SRTP
#endif

/* ── PSA Crypto оставляем ─────────────────────────────────────── */
/* ВАЖНО: НЕ трогаем:
 *   MBEDTLS_PSA_CRYPTO_C
 *   MBEDTLS_PSA_CRYPTO_CLIENT
 *   MBEDTLS_PSA_CRYPTO_CONFIG
 *   MBEDTLS_USE_PSA_CRYPTO
 * Они нужны mbedTLS 3.x для ECDHE и X.509.
 */

/* ── Отключаем PSA storage (у нас нет файловой ITS) ───────────── */
#ifdef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#endif
#ifdef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#endif

/* ── Размеры буферов SSL ──────────────────────────────────────── */
#ifdef MBEDTLS_SSL_IN_CONTENT_LEN
#undef MBEDTLS_SSL_IN_CONTENT_LEN
#endif
#define MBEDTLS_SSL_IN_CONTENT_LEN  4096

#ifdef MBEDTLS_SSL_OUT_CONTENT_LEN
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#endif
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

/* ── Embedded-опции для STM32 ─────────────────────────────────── */
/* Нет /dev/urandom и WinAPI — используем свой источник энтропии  */
#ifndef MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_NO_PLATFORM_ENTROPY
#endif

#ifndef MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#endif

/* mbedtls/platform_util.c требует ms_time на embedded без POSIX/Win */
#ifndef MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#endif

/* Не используем mbedtls_net_xxx, timing, FS I/O */
#ifdef MBEDTLS_NET_C
#undef MBEDTLS_NET_C
#endif
#ifdef MBEDTLS_TIMING_C
#undef MBEDTLS_TIMING_C
#endif
#ifdef MBEDTLS_FS_IO
#undef MBEDTLS_FS_IO
#endif

#endif /* MONSET_MBEDTLS_CONFIG_H */
