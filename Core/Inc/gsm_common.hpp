/**
 * @file gsm_common.hpp
 * @brief Общие типы для GSM/LTE драйверов (Air780E, SIM7020C).
 */
#pragma once
#include <cstdint>

enum class GsmStatus : uint8_t {
    Ok      = 0,
    Timeout = 1,
    NoSim   = 2,
    NoReg   = 3,
    PdnErr  = 4,
    TcpErr  = 5,
    HttpErr = 6,
    MqttErr = 7,
};
