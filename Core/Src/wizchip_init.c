/*
 * wizchip_init.c
 */

#include "wizchip_init.h"
#include "dbg_cwrap.h"

void WIZCHIPInitialize(void)
{
  csDisable();

  reg_wizchip_spi_cbfunc(spiReadByte, spiWriteByte);
  reg_wizchip_cs_cbfunc(csEnable, csDisable);

  uint8_t tmp = 0;

#if _WIZCHIP_SOCK_NUM_ == 8
  uint8_t memsize[2][8] = {
      {2,2,2,2,2,2,2,2},
      {2,2,2,2,2,2,2,2}
  };
#else
  uint8_t memsize[2][4] = {
      {2,2,2,2},
      {2,2,2,2}
  };
#endif

  if (ctlwizchip(CW_INIT_WIZCHIP, (void*)memsize) == -1) {
    dbg_puts_ln("W5500: init fail");
    return;
  }
  dbg_puts_ln("W5500: init OK");

  /* PHY link status check */
  do {
    if (ctlwizchip(CW_GET_PHYLINK, (void*)&tmp) == -1) {
      dbg_puts_ln("W5500: PHY status read fail");
      return;
    }
  } while (tmp == PHY_LINK_OFF);

  dbg_puts_ln("W5500: PHY link ON");
}

void csEnable(void)
{
  HAL_GPIO_WritePin(WIZCHIP_CS_PORT, WIZCHIP_CS_PIN, GPIO_PIN_RESET);
}

void csDisable(void)
{
  HAL_GPIO_WritePin(WIZCHIP_CS_PORT, WIZCHIP_CS_PIN, GPIO_PIN_SET);
}

void spiWriteByte(uint8_t tx)
{
  uint8_t rx;
  HAL_SPI_TransmitReceive(&WIZCHIP_SPI, &tx, &rx, 1, 10);
}

uint8_t spiReadByte(void)
{
  uint8_t rx = 0, tx = 0xFF;
  HAL_SPI_TransmitReceive(&WIZCHIP_SPI, &tx, &rx, 1, 10);
  return rx;
}
