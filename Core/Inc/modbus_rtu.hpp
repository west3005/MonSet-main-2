/**
 * ================================================================
 * @file    modbus_rtu.hpp
 * @brief   C++ класс Modbus RTU Master (USART3 + RS-485).
 * ================================================================
 */
#ifndef MODBUS_RTU_HPP
#define MODBUS_RTU_HPP

#include "main.h"
#include "config.hpp"
#include <cstdint>
#include <cstring>

enum class ModbusStatus : uint8_t {
    Ok       = 0,
    Timeout  = 1,
    CrcError = 2,
    SlaveErr = 3,
    FrameErr = 4
};

class ModbusRTU {
public:
    /**
     * @brief  Конструктор
     * @param  uart — USART для RS-485 (huart3)
     * @param  dePort — порт DE/RE пина
     * @param  dePin  — номер пина DE/RE
     */
    ModbusRTU(UART_HandleTypeDef* uart,
              GPIO_TypeDef* dePort, uint16_t dePin);

    /** Инициализация (DE/RE в режим приёма) */
    void init();

    /**
     * @brief  Чтение регистров
     * @param  slave   — адрес (1..247)
     * @param  fc      — функция (3 или 4)
     * @param  start   — начальный регистр
     * @param  count   — количество
     * @param  outRegs — массив для результата
     * @param  timeout — мс
     * @retval Статус
     */
    ModbusStatus readRegisters(uint8_t slave, uint8_t fc,
                                uint16_t start, uint16_t count,
                                uint16_t* outRegs,
                                uint32_t timeout = Config::MODBUS_TIMEOUT_MS);

    /** Статический расчёт CRC16 */
    static uint16_t crc16(const uint8_t* data, uint16_t len);

private:
    UART_HandleTypeDef* m_uart;
    GPIO_TypeDef*       m_dePort;
    uint16_t            m_dePin;

    void setTransmit();
    void setReceive();
};

#endif /* MODBUS_RTU_HPP */
