/**
 * @file    circular_log.cpp
 * @brief   Implementation of CircularLogBuffer — interrupt-safe circular log.
 *
 * Storage layout:
 *   m_buf[MAX_LINES][LINE_SIZE]  — ring buffer in .bss (~64 KB)
 *   m_head                       — index of next write slot
 *   m_count                      — number of valid entries (0..MAX_LINES)
 *   m_total                      — cumulative lines ever written
 *
 * Oldest-first indexing (getLine):
 *   When the buffer is full (m_count == MAX_LINES), the oldest entry sits at
 *   m_head (the slot that will be overwritten next).  For a partial buffer the
 *   oldest entry is always at slot 0.
 *
 *   oldest_slot = (m_count == MAX_LINES) ? m_head : 0
 *   actual_slot = (oldest_slot + idx) % MAX_LINES
 */

#include "circular_log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

// STM32 HAL header provides __disable_irq() / __enable_irq() via CMSIS.
// Guard with __has_include so unit-tests on a host machine still compile.
#if __has_include("stm32f4xx_hal.h")
#  include "stm32f4xx_hal.h"
#else
// Stub implementations for host-side unit testing
static inline void __disable_irq() {}
static inline void __enable_irq()  {}
#endif

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

CircularLogBuffer& CircularLogBuffer::instance()
{
    // Placed in .bss — zero-initialised by the C runtime, no dynamic allocation.
    static CircularLogBuffer s_instance;
    return s_instance;
}

// ---------------------------------------------------------------------------
// write()
// ---------------------------------------------------------------------------

void CircularLogBuffer::write(const char* level, const char* msg)
{
    // Build the formatted line into a local buffer before entering the
    // critical section so the IRQ gate is held for as short a time as
    // possible.
    char line[LINE_SIZE];

    // "[HH:MM:SS] LEVEL msg"
    // snprintf always null-terminates and truncates cleanly at LINE_SIZE-1.
    int prefixLen = snprintf(line, LINE_SIZE,
                             "[%02u:%02u:%02u] %s %s",
                             static_cast<unsigned>(m_hh),
                             static_cast<unsigned>(m_mm),
                             static_cast<unsigned>(m_ss),
                             level ? level : "    ",
                             msg   ? msg   : "");

    // Ensure null termination even if snprintf hit the limit.
    if (prefixLen < 0 || prefixLen >= static_cast<int>(LINE_SIZE)) {
        line[LINE_SIZE - 1] = '\0';
    }

    // --- Critical section: update shared indices ---
    __disable_irq();

    // Copy into ring buffer slot.
    memcpy(m_buf[m_head], line, LINE_SIZE);
    // Guarantee null termination in the stored slot.
    m_buf[m_head][LINE_SIZE - 1] = '\0';

    // Advance write pointer circularly.
    m_head = static_cast<uint16_t>((m_head + 1u) % MAX_LINES);

    // Saturate count at MAX_LINES (buffer full).
    if (m_count < MAX_LINES) {
        ++m_count;
    }

    // Total is unbounded (32-bit wraps naturally after ~4 billion lines).
    ++m_total;

    __enable_irq();
}

// ---------------------------------------------------------------------------
// writef()
// ---------------------------------------------------------------------------

void CircularLogBuffer::writef(const char* level, const char* fmt, ...)
{
    // Expand the format string into a temporary buffer, then delegate to write().
    char tmp[LINE_SIZE - 12]; // reserve space for "[HH:MM:SS] LEVEL " prefix

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt ? fmt : "", ap);
    va_end(ap);

    // Ensure null termination in case vsnprintf truncated.
    if (n < 0 || n >= static_cast<int>(sizeof(tmp))) {
        tmp[sizeof(tmp) - 1] = '\0';
    }

    write(level, tmp);
}

// ---------------------------------------------------------------------------
// getLine()
// ---------------------------------------------------------------------------

bool CircularLogBuffer::getLine(uint16_t idx, char* out, size_t outSz) const
{
    if (out == nullptr || outSz == 0) {
        return false;
    }

    // Snapshot volatile-sensitive fields outside the critical section.
    // Read is done without disabling IRQs because getLine() is intended for
    // the main-loop / FatFS path, and a torn read on a 16/32-bit aligned
    // Cortex-M4 is not possible for individually naturally-aligned types.
    // (m_count and m_head are each a single naturally-aligned word.)
    const uint16_t count = m_count;
    const uint16_t head  = m_head;

    if (idx >= count) {
        // Index out of range.
        out[0] = '\0';
        return false;
    }

    // Oldest slot:
    //   - Buffer not yet full (count < MAX_LINES): oldest is slot 0.
    //   - Buffer full (count == MAX_LINES):         oldest is m_head (next
    //     write will overwrite it, so it is currently the oldest).
    uint16_t oldestSlot = (count == MAX_LINES)
                              ? head
                              : static_cast<uint16_t>(0u);

    uint16_t actualSlot = static_cast<uint16_t>(
        (static_cast<uint32_t>(oldestSlot) + idx) % MAX_LINES);

    // Safe copy — strncpy pads with '\0' if src is shorter than outSz.
    strncpy(out, m_buf[actualSlot], outSz);
    out[outSz - 1] = '\0'; // guarantee null termination regardless of outSz

    return true;
}

// ---------------------------------------------------------------------------
// getCount()
// ---------------------------------------------------------------------------

uint16_t CircularLogBuffer::getCount() const
{
    return m_count;
}

// ---------------------------------------------------------------------------
// clear()
// ---------------------------------------------------------------------------

void CircularLogBuffer::clear()
{
    __disable_irq();
    m_head  = 0;
    m_count = 0;
    // m_total intentionally NOT reset — preserves lifetime write count.
    __enable_irq();
}
