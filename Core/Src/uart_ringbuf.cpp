#include "uart_ringbuf.hpp"

UartRingBuf<512> g_air780_rxbuf;

/* C-обёртка для вызова из ISR в .c файле */
extern "C" void air780_rxbuf_push(uint8_t byte)
{
    g_air780_rxbuf.push(byte);
}
