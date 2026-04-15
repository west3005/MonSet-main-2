#pragma once
#include "stm32f4xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void W5500_PortInit(SPI_HandleTypeDef *hspi);
void W5500_HardReset(void);

void wizchip_select(void);
void wizchip_deselect(void);
uint8_t wizchip_read(void);
void wizchip_write(uint8_t wb);
void wizchip_readburst(uint8_t *pBuf, uint16_t len);
void wizchip_writeburst(uint8_t *pBuf, uint16_t len);

uint32_t W5500_Millis(void);

#ifdef __cplusplus
}
#endif
