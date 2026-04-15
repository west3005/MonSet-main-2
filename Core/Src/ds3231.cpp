/**
 * ================================================================
 * @file    ds3231.cpp
 * @brief   Реализация DS3231 + синхронизация по SIM800 (AT+CCLK).
 * ================================================================
 */
#include "ds3231.hpp"
#include "debug_uart.hpp"

DS3231::DS3231(I2C_HandleTypeDef* i2c)
    : m_i2c(i2c)
{
}

/* ======== BCD ======== */
uint8_t DS3231::bcd2dec(uint8_t bcd) {
    return ((bcd >> 4) * 10U) + (bcd & 0x0FU);
}

uint8_t DS3231::dec2bcd(uint8_t dec) {
    return (uint8_t)(((dec / 10U) << 4) | (dec % 10U));
}

/* ======== Calendar helpers ======== */
bool DS3231::isLeap(uint16_t y)
{
    // 2000..2099: достаточно правила (делится на 4)
    return (y % 4U) == 0U;
}

uint8_t DS3231::daysInMonth(uint16_t y, uint8_t m)
{
    static const uint8_t mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m < 1 || m > 12) return 31;
    uint8_t d = mdays[m - 1];
    if (m == 2 && isLeap(y)) d = 29;
    return d;
}

// Sakamoto algorithm (0=Sunday..6=Saturday) -> map to 1..7
uint8_t DS3231::calcDOW(uint16_t y, uint8_t m, uint8_t d)
{
    static const uint8_t t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    uint16_t w = (uint16_t)(y + y/4 - y/100 + y/400 + t[m-1] + d) % 7U;
    return (uint8_t)(w + 1U); // Sunday=1
}

void DS3231::addMinutes(DateTime& dt, int minutesDelta)
{
    int total = (int)dt.hours * 60 + (int)dt.minutes + minutesDelta;

    while (total < 0) {
        // шаг назад на сутки
        total += 1440;

        // дата - 1 день
        uint16_t y = dt.yearFull();
        if (dt.date > 1) {
            dt.date--;
        } else {
            if (dt.month > 1) {
                dt.month--;
            } else {
                dt.month = 12;
                if (dt.year > 0) dt.year--;
                y = dt.yearFull();
            }
            dt.date = daysInMonth(y, dt.month);
        }
    }

    while (total >= 1440) {
        total -= 1440;

        // дата + 1 день
        uint16_t y = dt.yearFull();
        uint8_t dim = daysInMonth(y, dt.month);
        if (dt.date < dim) {
            dt.date++;
        } else {
            dt.date = 1;
            if (dt.month < 12) {
                dt.month++;
            } else {
                dt.month = 1;
                dt.year++;
            }
        }
    }

    dt.hours = (uint8_t)(total / 60);
    dt.minutes = (uint8_t)(total % 60);
}

/* ======== Init ======== */
bool DS3231::init()
{
    auto status = HAL_I2C_IsDeviceReady(m_i2c, Config::DS3231_ADDR, 3, 100);

    if (status == HAL_OK) {
        DateTime now;
        if (getTime(now)) {
            DBG.info("DS3231: OK. Время: 20%02u-%02u-%02u %02u:%02u:%02u",
                     (unsigned)now.year, (unsigned)now.month, (unsigned)now.date,
                     (unsigned)now.hours, (unsigned)now.minutes, (unsigned)now.seconds);
        }
        DBG.info("DS3231: Температура: %.2f C", getTemperature());
        return true;
    }

    DBG.error("DS3231: НЕ ОТВЕЧАЕТ на I2C!");
    return false;
}

/* ======== Read time ======== */
bool DS3231::getTime(DateTime& dt)
{
    uint8_t buf[7];
    auto s = HAL_I2C_Mem_Read(m_i2c, Config::DS3231_ADDR,
                             0x00, I2C_MEMADD_SIZE_8BIT,
                             buf, 7, 100);
    if (s != HAL_OK) return false;

    dt.seconds = bcd2dec(buf[0] & 0x7F);
    dt.minutes = bcd2dec(buf[1] & 0x7F);
    dt.hours   = bcd2dec(buf[2] & 0x3F);
    dt.day     = bcd2dec(buf[3] & 0x07);
    dt.date    = bcd2dec(buf[4] & 0x3F);
    dt.month   = bcd2dec(buf[5] & 0x1F);
    dt.year    = bcd2dec(buf[6]);

    return true;
}

/* ======== Set time ======== */
bool DS3231::setTime(const DateTime& dt)
{
    uint8_t buf[7] = {
        dec2bcd(dt.seconds),
        dec2bcd(dt.minutes),
        dec2bcd(dt.hours),
        dec2bcd(dt.day),
        dec2bcd(dt.date),
        dec2bcd(dt.month),
        dec2bcd(dt.year)
    };

    return HAL_I2C_Mem_Write(m_i2c, Config::DS3231_ADDR,
                            0x00, I2C_MEMADD_SIZE_8BIT,
                            buf, 7, 100) == HAL_OK;
}

bool DS3231::setTimeAutoDOW(DateTime dt)
{
    dt.day = calcDOW(dt.yearFull(), dt.month, dt.date);
    return setTime(dt);
}

/* ======== Temperature ======== */
float DS3231::getTemperature()
{
    uint8_t buf[2];
    HAL_I2C_Mem_Read(m_i2c, Config::DS3231_ADDR,
                     0x11, I2C_MEMADD_SIZE_8BIT, buf, 2, 100);
    return (float)(int8_t)buf[0] + (float)(buf[1] >> 6) * 0.25f;
}

/* ======== SIM800 CCLK parsing ======== */
const char* DS3231::findQuotedTime(const char* s)
{
    if (!s) return nullptr;

    // Ищем первую кавычку "
    const char* q = std::strchr(s, '\"');
    if (!q) return nullptr;
    return q + 1; // после "
}

bool DS3231::parseCCLKFields(const char* s,
                             uint8_t& yy, uint8_t& MM, uint8_t& dd,
                             uint8_t& hh, uint8_t& mm, uint8_t& ss,
                             int& tzQuarters)
{
    // Ожидаем: yy/MM/dd,hh:mm:ss±zz
    // tzQuarters: signed integer (zz), unit = 15 minutes
    if (!s) return false;

    int iyy=0, iMM=0, idd=0, ihh=0, imm=0, iss=0, itz=0;
    char sign = '+';

    // Пример: 19/05/25,21:50:06+12
    int n = std::sscanf(s, "%2d/%2d/%2d,%2d:%2d:%2d%c%2d",
                        &iyy, &iMM, &idd, &ihh, &imm, &iss, &sign, &itz);
    if (n < 7) return false;

    if (sign != '+' && sign != '-') return false;

    yy = (uint8_t)iyy;
    MM = (uint8_t)iMM;
    dd = (uint8_t)idd;
    hh = (uint8_t)ihh;
    mm = (uint8_t)imm;
    ss = (uint8_t)iss;

    int tz = itz;
    if (sign == '-') tz = -tz;
    tzQuarters = tz;

    if (MM < 1 || MM > 12) return false;
    if (dd < 1 || dd > 31) return false;
    if (hh > 23 || mm > 59 || ss > 59) return false;

    return true;
}

bool DS3231::parseSim800CCLK_UTC(const char* cclkLine, DateTime& outUtc)
{
    if (!cclkLine) return false;

    // В ответе часто: +CCLK: "yy/MM/dd,hh:mm:ss+zz"
    // поэтому вытаскиваем содержимое в кавычках
    const char* p = findQuotedTime(cclkLine);
    if (!p) {
        // может быть строка уже без +CCLK:
        p = cclkLine;
    }

    uint8_t yy, MM, dd, hh, mm, ss;
    int tzQ = 0;

    if (!parseCCLKFields(p, yy, MM, dd, hh, mm, ss, tzQ)) return false;

    DateTime local{};
    local.year = yy;
    local.month = MM;
    local.date = dd;
    local.hours = hh;
    local.minutes = mm;
    local.seconds = ss;

    // В AT+CCLK? время локальное, tz = local - UTC (в четвертях часа). [web:157]
    // Значит UTC = local - tz
    int tzMinutes = tzQ * 15;
    DateTime utc = local;
    addMinutes(utc, -tzMinutes);

    utc.day = calcDOW(utc.yearFull(), utc.month, utc.date);
    outUtc = utc;
    return true;
}

bool DS3231::setTimeFromSim800CCLK(const char* cclkLine)
{
    DateTime utc;
    if (!parseSim800CCLK_UTC(cclkLine, utc)) {
        DBG.error("DS3231: parse CCLK failed");
        return false;
    }

    if (!setTimeAutoDOW(utc)) {
        DBG.error("DS3231: setTime failed");
        return false;
    }

    DBG.info("DS3231: synced from GSM, UTC: 20%02u-%02u-%02u %02u:%02u:%02u",
             (unsigned)utc.year, (unsigned)utc.month, (unsigned)utc.date,
             (unsigned)utc.hours, (unsigned)utc.minutes, (unsigned)utc.seconds);
    return true;
}
