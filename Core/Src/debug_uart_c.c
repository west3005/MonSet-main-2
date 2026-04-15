/*
 * debug_uart_c.c
 *
 * Created on: Mar 9, 2026
 * Author: My
 */
#include "debug_uart_c.h"
#include <stdarg.h>
#include <stdio.h>
#include "main.h"

static void _uart_log(const char *level, const char *fmt, va_list ap)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "[%s] ", level);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);

    /* Append newline */
    int len = 0;
    while (buf[len] && len < (int)sizeof(buf) - 2) len++;
    buf[len++] = '\r';
    buf[len++] = '\n';
    buf[len]   = '\0';

    HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)len, 100);
}

void uart_log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _uart_log("INF", fmt, ap);
    va_end(ap);
}

void uart_log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _uart_log("WRN", fmt, ap);
    va_end(ap);
}

void uart_log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _uart_log("ERR", fmt, ap);
    va_end(ap);
}
