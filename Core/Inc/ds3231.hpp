/**
 * ================================================================
 * @file    ds3231.hpp
 * @brief   C++ класс для DS3231 RTC (I2C1) + парсинг времени SIM800 (AT+CCLK).
 * ================================================================
 */
#ifndef DS3231_HPP
#define DS3231_HPP

#include "main.h"
#include "config.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>

struct DateTime {
    uint8_t seconds  = 0;
    uint8_t minutes  = 0;
    uint8_t hours    = 0;
    uint8_t day      = 1;   // DS3231 day-of-week (1..7), выставим автоматически
    uint8_t date     = 1;   // day of month 1..31
    uint8_t month    = 1;   // 1..12
    uint8_t year     = 24;  // 00..99 meaning 2000..2099

    uint16_t yearFull() const { return (uint16_t)(2000u + (uint16_t)year); }

    void formatISO8601(char* buf) const {
        std::snprintf(buf, 32, "20%02u-%02u-%02uT%02u:%02u:%02u.000Z",
                      (unsigned)year, (unsigned)month, (unsigned)date,
                      (unsigned)hours, (unsigned)minutes, (unsigned)seconds);
    }
};

class DS3231 {
public:
    explicit DS3231(I2C_HandleTypeDef* i2c);

    bool init();
    bool getTime(DateTime& dt);
    bool setTime(const DateTime& dt);                 // выставляет как есть (day берёт из dt)
    bool setTimeAutoDOW(DateTime dt);                 // вычислит day и запишет
    float getTemperature();

    // Парсинг ответа SIM800 на AT+CCLK? и выставление DS3231 в UTC.
    // Принимает строку вида:
    //   +CCLK: "yy/MM/dd,hh:mm:ss±zz"
    // либо просто:
    //   "yy/MM/dd,hh:mm:ss±zz"
    bool setTimeFromSim800CCLK(const char* cclkLine);

    // Полезно для отладки: распарсить CCLK в UTC, не трогая железо.
    static bool parseSim800CCLK_UTC(const char* cclkLine, DateTime& outUtc);

private:
    I2C_HandleTypeDef* m_i2c;

    static uint8_t bcd2dec(uint8_t bcd);
    static uint8_t dec2bcd(uint8_t dec);

    static bool isLeap(uint16_t y);
    static uint8_t daysInMonth(uint16_t y, uint8_t m);

    // Возвращает 1..7 (Sunday=1..Saturday=7) — для регистра DS3231
    static uint8_t calcDOW(uint16_t y, uint8_t m, uint8_t d);

    // dt += minutesDelta (может быть отрицательным), корректируя дату
    static void addMinutes(DateTime& dt, int minutesDelta);

    static bool parseCCLKFields(const char* s,
                                uint8_t& yy, uint8_t& MM, uint8_t& dd,
                                uint8_t& hh, uint8_t& mm, uint8_t& ss,
                                int& tzQuarters);

    static const char* findQuotedTime(const char* s);
};

#endif /* DS3231_HPP */
