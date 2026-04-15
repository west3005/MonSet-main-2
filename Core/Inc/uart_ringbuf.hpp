/**
 * @file uart_ringbuf.hpp
 * @brief Кольцевой буфер для UART RX (заполняется из ISR).
 *        Совместим с C и C++ (используется из stm32f4xx_it.c).
 */
#pragma once

#ifdef __cplusplus
#include <cstdint>
#include <cstring>
#else
#include <stdint.h>
#include <string.h>
#endif

#ifdef __cplusplus

template<uint16_t SIZE>
class UartRingBuf {
public:
    void push(uint8_t byte) {
        uint16_t next = (m_head + 1) % SIZE;
        if (next != m_tail) {
            m_buf[m_head] = byte;
            m_head = next;
        }
    }

    bool pop(uint8_t& byte) {
        if (m_tail == m_head) return false;
        byte = m_buf[m_tail];
        m_tail = (m_tail + 1) % SIZE;
        return true;
    }

    bool empty() const { return m_head == m_tail; }
    void clear()       { m_head = m_tail = 0; }

private:
    volatile uint8_t  m_buf[SIZE]{};
    volatile uint16_t m_head = 0;
    volatile uint16_t m_tail = 0;
};

extern UartRingBuf<512> g_air780_rxbuf;

#endif /* __cplusplus */

/* C-совместимый интерфейс для использования из stm32f4xx_it.c */
#ifdef __cplusplus
extern "C" {
#endif

void air780_rxbuf_push(uint8_t byte);

#ifdef __cplusplus
}
#endif
