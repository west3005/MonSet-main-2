#ifndef DEBUG_UART_C_H
#define DEBUG_UART_C_H

/**
 * @file  debug_uart_c.h
 * @brief C-compatible logging wrappers for use in .c files (e.g. sd_diskio.c).
 *
 * The actual implementation lives in debug_uart.cpp and is compiled as C++.
 * These declarations use extern "C" so the linker resolves them correctly
 * whether called from C or C++ translation units.
 *
 * Usage in a .c file:
 *   #include "debug_uart_c.h"
 *   uart_log_info("hello %d", 42);
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Log at INFO level. printf-style format string.
 *         Maps to DBG.info() internally.
 */
void uart_log_info(const char *fmt, ...);

/**
 * @brief  Log at WARN level.
 *         Maps to DBG.warn() internally.
 */
void uart_log_warn(const char *fmt, ...);

/**
 * @brief  Log at ERROR level.
 *         Maps to DBG.error() internally.
 */
void uart_log_error(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_UART_C_H */
