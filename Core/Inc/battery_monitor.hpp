/**
 * ================================================================
 * @file    battery_monitor.hpp
 * @brief   Battery voltage monitor via ADC.
 *
 * @note    Uses PA4 (ADC1_CH4) with voltage divider for
 *          24V LiFePO4 battery pack (8S: 20V=0%, 26V=100%).
 *          Voltage divider assumed: R1=100k, R2=10k → ratio 11:1.
 *          ADC Vref = 3.3V, 12-bit (4096 counts).
 * ================================================================
 */
#pragma once

#include "stm32f4xx_hal.h"
#include <cstdint>

class BatteryMonitor {
public:
    BatteryMonitor();

    /// @brief Initialize ADC for battery voltage reading
    bool init();

    /// @brief Read battery voltage (call periodically)
    void update();

    /// @brief Get last measured voltage (V)
    float getVoltage() const { return m_voltage; }

    /// @brief Get battery percentage (0-100)
    uint8_t getPercent() const { return m_percent; }

    /// @brief Check if battery is below threshold
    bool isLow() const;

    /// @brief Get raw ADC value
    uint16_t getRawAdc() const { return m_rawAdc; }

private:
    ADC_HandleTypeDef m_hadc;
    float    m_voltage  = 0.0f;
    uint8_t  m_percent  = 0;
    uint16_t m_rawAdc   = 0;
    bool     m_initialized = false;

    /// Voltage divider ratio (R1+R2)/R2
    /// With R1=100k, R2=10k: ratio = 11.0
    static constexpr float VDIV_RATIO = 11.0f;

    /// ADC reference voltage
    static constexpr float VREF = 3.3f;

    /// ADC resolution (12-bit)
    static constexpr uint16_t ADC_MAX = 4095;

    /// LiFePO4 8S voltage range
    static constexpr float VBAT_MIN = 20.0f;  ///< 0% SoC
    static constexpr float VBAT_MAX = 26.0f;  ///< 100% SoC

    /// @brief Convert ADC raw value to battery voltage
    float adcToVoltage(uint16_t raw) const;

    /// @brief Convert voltage to SoC percentage
    uint8_t voltageToPercent(float voltage) const;
};
