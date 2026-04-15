/**
 * ================================================================
 * @file    data_buffer.cpp
 * @brief   Реализация буфера измерений.
 * ================================================================
 */
#include "data_buffer.hpp"
#include "debug_uart.hpp"

void DataBuffer::add(float value, const DateTime& dt)
{
    if (m_count >= Config::MEAS_BUFFER_SIZE) {
        DBG.warn("Буфер переполнен! Перезапись.");
        m_count = 0;
    }

    auto& m = m_data[m_count];
    m.value     = value;
    m.timestamp = dt;
    m.valid     = true;
    m_count++;

    DBG.info("Буфер: %d/%d", m_count,
             static_cast<int>(Config::SEND_INTERVAL_POLLS));
}

uint16_t DataBuffer::toJSON(char* buf, uint16_t bsz) const
{
    uint16_t off = 0;
    off += std::snprintf(buf + off, bsz - off, "[");

    bool first = true;
    for (uint8_t i = 0; i < m_count; i++) {
        if (!m_data[i].valid) continue;

        if (!first) off += std::snprintf(buf + off, bsz - off, ",");
        first = false;

        const auto& t = m_data[i].timestamp;
        off += std::snprintf(buf + off, bsz - off,
            "{\"metricId\":\"%s\","
            "\"value\":%.3f,"
            "\"measureTime\":\"20%02u-%02u-%02uT%02u:%02u:%02u.000Z\"}",
            Config::METRIC_ID,
            static_cast<double>(m_data[i].value),
            static_cast<unsigned>(t.year),
            static_cast<unsigned>(t.month),
            static_cast<unsigned>(t.date),
            static_cast<unsigned>(t.hours),
            static_cast<unsigned>(t.minutes),
            static_cast<unsigned>(t.seconds));

        if (off >= bsz - 16) { DBG.warn("JSON обрезан!"); break; }
    }

    off += std::snprintf(buf + off, bsz - off, "]");
    return off;
}

void DataBuffer::clear()
{
    /* Вместо memset — поэлементный сброс (безопасно для non-trivial типов) */
    for (uint8_t i = 0; i < Config::MEAS_BUFFER_SIZE; i++) {
        m_data[i] = Measurement{};
    }
    m_count = 0;
    DBG.info("Буфер очищен");
}
