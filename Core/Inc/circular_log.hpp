/**
 * @file    circular_log.hpp
 * @brief   Interrupt-safe circular log buffer — 500 lines × 128 bytes (~64 KB in .bss)
 *
 * API:
 *   CircularLogBuffer::instance()        — singleton
 *   .write(level, msg)                   — interrupt-safe push (disable/enable IRQ)
 *   .getLine(idx, out, outSz)            — read line by absolute index
 *   .getCount()                          — total lines written (wrapping)
 *   .getTotal()                          — total ever written (for offset tracking)
 *   .clear()                             — reset head/tail
 *
 * Log format per line: "[HH:MM:SS] LEVEL msg\0" (max 127 chars + null)
 * Level values: "INFO", "WARN", "ERR ", "DATA"
 *
 * Interrupt safety: write() disables/re-enables IRQs around index update.
 * FatFS operations NEVER done here — only from main loop via exported plain-text API.
 */
#pragma once
#include <cstdint>
#include <cstddef>

class CircularLogBuffer {
public:
    static constexpr uint16_t MAX_LINES  = 500;
    static constexpr uint8_t  LINE_SIZE  = 128;

    static CircularLogBuffer& instance();

    /** @brief Must be called once early in main() to clear CCMRAM pointers. */
    static void init_ccmram();

    /** @brief Interrupt-safe write. Truncates at LINE_SIZE-1. */
    void write(const char* level, const char* msg);

    /** @brief Printf-style write */
    void writef(const char* level, const char* fmt, ...);

    /** @brief Read line by sequential index (0 = oldest). Returns false if out of range. */
    bool getLine(uint16_t idx, char* out, size_t outSz) const;

    /** @brief Number of lines currently in buffer (0..MAX_LINES) */
    uint16_t getCount() const;

    /** @brief Total lines ever written (for client-side offset tracking) */
    uint32_t getTotal() const { return m_total; }

    /** @brief Clear buffer */
    void clear();

    /** @brief Set current time for log timestamps (call after NTP sync) */
    void setTime(uint8_t h, uint8_t m, uint8_t s) {
        m_hh = h; m_mm = m; m_ss = s;
    }

private:
    CircularLogBuffer() = default;

    char     m_buf[MAX_LINES][LINE_SIZE]{};
    uint16_t m_head  = 0;   // next write position
    uint16_t m_count = 0;   // lines in buffer
    uint32_t m_total = 0;   // total ever written

    uint8_t  m_hh = 0, m_mm = 0, m_ss = 0;
};

// Global convenience macros (use DBG_LOG for interrupt-safe log)
#define LOG_INFO(msg)  CircularLogBuffer::instance().write("INFO", msg)
#define LOG_WARN(msg)  CircularLogBuffer::instance().write("WARN", msg)
#define LOG_ERR(msg)   CircularLogBuffer::instance().write("ERR ", msg)
