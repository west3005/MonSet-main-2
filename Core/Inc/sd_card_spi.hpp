/**
 * ================================================================
 * @file    sd_card_spi.hpp
 * @brief   Низкоуровневый SPI-драйвер SD-карты.
 * ================================================================
 */
#ifndef SD_CARD_SPI_HPP
#define SD_CARD_SPI_HPP

#include "main.h"
#include "config.hpp"
#include <cstdint>

enum class SdCardType : uint8_t {
    Unknown = 0,
    SDv1    = 1,
    SDv2    = 2,
    SDHC    = 3
};

class SdCardSPI {
public:
    SdCardSPI(SPI_HandleTypeDef* spi,
              GPIO_TypeDef* csPort, uint16_t csPin);

    bool       init();
    bool       readBlock(uint32_t sector, uint8_t* buf);
    bool       writeBlock(uint32_t sector, const uint8_t* buf);
    SdCardType getType() const { return m_type; }

private:
    SPI_HandleTypeDef* m_spi;
    GPIO_TypeDef*      m_csPort;
    uint16_t           m_csPin;
    SdCardType         m_type = SdCardType::Unknown;

    void    csLow();
    void    csHigh();
    uint8_t spiXfer(uint8_t tx);
    uint8_t waitReady(uint32_t timeout);
    uint8_t sendCmd(uint8_t cmd, uint32_t arg);
};

#endif /* SD_CARD_SPI_HPP */
