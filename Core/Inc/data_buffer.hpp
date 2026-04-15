/**
 * ================================================================
 * @file    data_buffer.hpp
 * @brief   Кольцевой буфер измерений + JSON-сериализация.
 * ================================================================
 */
#ifndef DATA_BUFFER_HPP
#define DATA_BUFFER_HPP

#include "config.hpp"
#include "ds3231.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>

/**
 * @brief  Одно измерение: значение + метка времени
 */
struct Measurement {
    float    value    = 0.0f;
    DateTime timestamp{};
    bool     valid    = false;
};

/**
 * @brief  Буфер измерений в ОЗУ.
 *         Данные НЕ теряются в Stop Mode (SRAM сохраняется).
 */
class DataBuffer {
public:
    DataBuffer() = default;

    /** Добавить измерение в буфер */
    void add(float value, const DateTime& dt);

    /** Количество измерений в буфере */
    uint8_t count() const { return m_count; }

    /** Буфер полон? (достигли SEND_INTERVAL) */
    bool isFull() const {
        return m_count >= Config::SEND_INTERVAL_POLLS;
    }

    /**
     * @brief  Сериализовать буфер в JSON массив
     * @param  buf  — выходной буфер
     * @param  bsz  — размер буфера
     * @retval Длина JSON строки
     */
    uint16_t toJSON(char* buf, uint16_t bsz) const;

    /** Очистить буфер */
    void clear();

private:
    Measurement m_data[Config::MEAS_BUFFER_SIZE]{};
    uint8_t     m_count = 0;
};

#endif /* DATA_BUFFER_HPP */
