/**
 * ================================================================
 * @file    sd_card_spi.cpp
 * @brief   SPI SD-карта: CMD0 → CMD8 → ACMD41 → CMD58.
 * ================================================================
 */
#include "sd_card_spi.hpp"
#include "debug_uart.hpp"

SdCardSPI::SdCardSPI(SPI_HandleTypeDef* spi,
                      GPIO_TypeDef* csPort, uint16_t csPin)
    : m_spi(spi), m_csPort(csPort), m_csPin(csPin)
{
}

void SdCardSPI::csLow()  { HAL_GPIO_WritePin(m_csPort, m_csPin, GPIO_PIN_RESET); }
void SdCardSPI::csHigh() { HAL_GPIO_WritePin(m_csPort, m_csPin, GPIO_PIN_SET);   }

uint8_t SdCardSPI::spiXfer(uint8_t tx) {
    uint8_t rx = 0xFF;
    HAL_SPI_TransmitReceive(m_spi, &tx, &rx, 1, 100);
    return rx;
}

uint8_t SdCardSPI::waitReady(uint32_t timeout) {
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout) {
        if (spiXfer(0xFF) == 0xFF) return 0;
    }
    return 1;
}

uint8_t SdCardSPI::sendCmd(uint8_t cmd, uint32_t arg)
{
    if (cmd & 0x80) {         /* ACMD */
        cmd &= 0x7F;
        uint8_t r = sendCmd(55, 0);
        if (r > 1) return r;
    }

    csHigh(); spiXfer(0xFF);
    csLow();  spiXfer(0xFF);

    spiXfer(0x40 | cmd);
    spiXfer(static_cast<uint8_t>(arg >> 24));
    spiXfer(static_cast<uint8_t>(arg >> 16));
    spiXfer(static_cast<uint8_t>(arg >> 8));
    spiXfer(static_cast<uint8_t>(arg));

    uint8_t crc = 0xFF;
    if (cmd == 0)  crc = 0x95;
    if (cmd == 8)  crc = 0x87;
    spiXfer(crc);

    uint8_t n = 10, r;
    do { r = spiXfer(0xFF); } while ((r & 0x80) && --n);
    return r;
}

bool SdCardSPI::init()
{
    m_type = SdCardType::Unknown;

    /* 80+ тактов с CS=HIGH */
    csHigh();
    for (int i = 0; i < 10; i++) spiXfer(0xFF);

    /* CMD0 */
    uint16_t retry = 100;
    uint8_t r;
    do { r = sendCmd(0, 0); } while (r != 0x01 && --retry);
    if (r != 0x01) { csHigh(); DBG.error("SD: CMD0 fail"); return false; }
    DBG.info("SD: CMD0 OK");

    /* CMD8 */
    r = sendCmd(8, 0x1AA);
    if (r == 0x01) {
        uint8_t ocr[4];
        for (auto& b : ocr) b = spiXfer(0xFF);

        if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
            retry = 1000;
            do { r = sendCmd(0x80 | 41, 0x40000000); HAL_Delay(1); }
            while (r && --retry);

            if (r == 0) {
                sendCmd(58, 0);
                for (auto& b : ocr) b = spiXfer(0xFF);
                m_type = (ocr[0] & 0x40) ? SdCardType::SDHC : SdCardType::SDv2;
            }
        }
    } else {
        retry = 1000;
        do { r = sendCmd(0x80 | 41, 0); HAL_Delay(1); }
        while (r && --retry);
        if (r == 0) m_type = SdCardType::SDv1;
    }

    csHigh(); spiXfer(0xFF);

    if (m_type == SdCardType::Unknown) {
        DBG.error("SD: init fail");
        return false;
    }

    const char* names[] = {"?", "SDv1", "SDv2", "SDHC"};
    DBG.info("SD: тип=%s", names[static_cast<int>(m_type)]);
    return true;
}

bool SdCardSPI::readBlock(uint32_t sector, uint8_t* buf)
{
    uint32_t addr = (m_type != SdCardType::SDHC) ? sector * 512 : sector;
    if (sendCmd(17, addr) != 0) { csHigh(); return false; }

    uint32_t start = HAL_GetTick();
    uint8_t r;
    do {
        r = spiXfer(0xFF);
        if ((HAL_GetTick() - start) > 500) { csHigh(); return false; }
    } while (r != 0xFE);

    for (int i = 0; i < 512; i++) buf[i] = spiXfer(0xFF);
    spiXfer(0xFF); spiXfer(0xFF);  /* CRC */
    csHigh(); spiXfer(0xFF);
    return true;
}

bool SdCardSPI::writeBlock(uint32_t sector, const uint8_t* buf)
{
    uint32_t addr = (m_type != SdCardType::SDHC) ? sector * 512 : sector;
    if (sendCmd(24, addr) != 0) { csHigh(); return false; }

    spiXfer(0xFF);
    spiXfer(0xFE);  /* Data Token */

    for (int i = 0; i < 512; i++) spiXfer(buf[i]);
    spiXfer(0xFF); spiXfer(0xFF);  /* CRC */

    uint8_t r = spiXfer(0xFF);
    if ((r & 0x1F) != 0x05) { csHigh(); return false; }

    waitReady(500);
    csHigh(); spiXfer(0xFF);
    return true;
}
