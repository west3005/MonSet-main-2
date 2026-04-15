#include "w5500_port.h"

// --- W5500 pins (оставляем #define как у вас для HAL) ---
#define PIN_W5500_CS_PORT GPIOA
#define PIN_W5500_CS_PIN  GPIO_PIN_8

#define PIN_W5500_RST_PORT GPIOC
#define PIN_W5500_RST_PIN  GPIO_PIN_3

#define PIN_W5500_INT_PORT GPIOC
#define PIN_W5500_INT_PIN  GPIO_PIN_4


// Выбор канала связи (DI с подтяжкой к VCC)
// Замкнут на GND -> ETH (W5500)
// Разомкнут -> GSM (SIM800L)
#define PIN_NET_SW_PORT GPIOB
#define PIN_NET_SW_PIN  GPIO_PIN_0


static SPI_HandleTypeDef *s_hspi = 0;

void W5500_PortInit(SPI_HandleTypeDef *hspi) { s_hspi = hspi; }
uint32_t W5500_Millis(void) { return HAL_GetTick(); }

void wizchip_select(void)   { HAL_GPIO_WritePin(PIN_W5500_CS_PORT, PIN_W5500_CS_PIN, GPIO_PIN_RESET); }
void wizchip_deselect(void) { HAL_GPIO_WritePin(PIN_W5500_CS_PORT, PIN_W5500_CS_PIN, GPIO_PIN_SET); }

uint8_t wizchip_read(void) {
  uint8_t tx = 0xFF, rx = 0;
  HAL_SPI_TransmitReceive(s_hspi, &tx, &rx, 1, 100);
  return rx;
}
void wizchip_write(uint8_t wb) { HAL_SPI_Transmit(s_hspi, &wb, 1, 100); }

void wizchip_readburst(uint8_t *pBuf, uint16_t len) {
  uint8_t tx = 0xFF;
  for (uint16_t i = 0; i < len; i++) HAL_SPI_TransmitReceive(s_hspi, &tx, &pBuf[i], 1, 200);
}
void wizchip_writeburst(uint8_t *pBuf, uint16_t len) {
  HAL_SPI_Transmit(s_hspi, pBuf, len, 200);
}

void W5500_HardReset(void) {
  HAL_GPIO_WritePin(PIN_W5500_RST_PORT, PIN_W5500_RST_PIN, GPIO_PIN_RESET);
  HAL_Delay(2);
  HAL_GPIO_WritePin(PIN_W5500_RST_PORT, PIN_W5500_RST_PIN, GPIO_PIN_SET);
  HAL_Delay(150);
}
