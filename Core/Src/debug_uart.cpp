/**
 * ================================================================
 * @file    debug_uart.cpp
 * @brief   Реализация отладочного UART (115200 8N1).
 * ================================================================
 */
#include "debug_uart.hpp"

DebugUart DBG(&huart1);

DebugUart::DebugUart(UART_HandleTypeDef* uart)
  : m_uart(uart), m_uartMirror(nullptr), m_buf{}, m_enabled(true) {}

void DebugUart::init() {
  HAL_Delay(100);

  separator();
  raw("MonSet boot\r\n");
  info("SYSCLK : %lu MHz", HAL_RCC_GetSysClockFreq() / 1000000UL);
  info("HCLK   : %lu MHz", HAL_RCC_GetHCLKFreq() / 1000000UL);
  info("APB1   : %lu MHz", HAL_RCC_GetPCLK1Freq() / 1000000UL);
  info("APB2   : %lu MHz", HAL_RCC_GetPCLK2Freq() / 1000000UL);
  info("Build  : %s %s", __DATE__, __TIME__);
  separator();
}

void DebugUart::info(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  send(LogLevel::Info, fmt, args);
  va_end(args);
}

void DebugUart::warn(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  send(LogLevel::Warn, fmt, args);
  va_end(args);
}

void DebugUart::error(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  send(LogLevel::Error, fmt, args);
  va_end(args);
}

void DebugUart::data(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  send(LogLevel::Data, fmt, args);
  va_end(args);
}

void DebugUart::raw(const char* str) {
  if (!m_enabled || !str) return;
  transmit(str, (uint16_t)std::strlen(str));
}

void DebugUart::separator() {
  raw("============================================================\r\n");
}

void DebugUart::hexDump(const char* label, const uint8_t* buf, uint16_t len) {
  if (!m_enabled) return;
  int off = std::snprintf(m_buf, sizeof(m_buf), "[%8lu] [DATA ] %s (%u bytes): ",
                          HAL_GetTick(), label ? label : "buf", (unsigned)len);
  for (uint16_t i = 0; i < len && off < (int)sizeof(m_buf) - 8; i++) {
    off += std::snprintf(m_buf + off, sizeof(m_buf) - (size_t)off, "%02X ", buf[i]);
  }
  if (off < (int)sizeof(m_buf) - 3) {
    m_buf[off++] = '\r';
    m_buf[off++] = '\n';
    m_buf[off] = 0;
  }
  transmit(m_buf, (uint16_t)off);
}

void DebugUart::send(LogLevel lvl, const char* fmt, va_list args) {
  if (!m_enabled) return;

  int hdr = std::snprintf(m_buf, sizeof(m_buf), "[%8lu] [%s] ",
                          HAL_GetTick(), levelStr(lvl));
  if (hdr < 0) return;

  int body = std::vsnprintf(m_buf + hdr, sizeof(m_buf) - (size_t)hdr - 4, fmt, args);
  if (body < 0) body = 0;

  int total = hdr + body;
  if (total > (int)sizeof(m_buf) - 4) total = (int)sizeof(m_buf) - 4;

  m_buf[total++] = '\r';
  m_buf[total++] = '\n';
  m_buf[total] = 0;

  transmit(m_buf, (uint16_t)total);
}

void DebugUart::transmit(const char* data, uint16_t len) {
  if (!data || len == 0) return;

  if (m_uart) {
    HAL_UART_Transmit(m_uart, (uint8_t*)data, len, 200);
  }
  if (m_uartMirror) {
    HAL_UART_Transmit(m_uartMirror, (uint8_t*)data, len, 200);
  }
}

const char* DebugUart::levelStr(LogLevel lvl) {
  switch (lvl) {
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Data:  return "DATA ";
    default:              return "?????";
  }
}
