/**
 * ================================================================
 * @file    debug_uart.hpp
 * @brief   Отладочный вывод через UART (основной) + опционально mirror UART.
 * Формат:  [tick_ms] [LEVEL] message\r\n
 * ================================================================
 */
#ifndef DEBUG_UART_HPP
#define DEBUG_UART_HPP

#include "main.h"
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

enum class LogLevel : uint8_t { Info = 0, Warn = 1, Error = 2, Data = 3 };

class DebugUart {
public:
  explicit DebugUart(UART_HandleTypeDef* uart);

  void init();

  void info(const char* fmt, ...);
  void warn(const char* fmt, ...);
  void error(const char* fmt, ...);
  void data(const char* fmt, ...);

  void raw(const char* str);
  void separator();
  void hexDump(const char* label, const uint8_t* buf, uint16_t len);

  void setEnabled(bool en) { m_enabled = en; }
  bool isEnabled() const { return m_enabled; }

  // +++ НОВОЕ: зеркалирование во второй UART (например USART6->ESP)
  void setMirror(UART_HandleTypeDef* uart) { m_uartMirror = uart; }

private:
  UART_HandleTypeDef* m_uart;
  UART_HandleTypeDef* m_uartMirror = nullptr;
  char m_buf[300];
  bool m_enabled;

  void send(LogLevel lvl, const char* fmt, va_list args);
  void transmit(const char* data, uint16_t len);
  static const char* levelStr(LogLevel lvl);
};

extern DebugUart DBG;

#endif // DEBUG_UART_HPP
