/**
 * ================================================================
 * @file    battery_monitor.cpp
 * @brief   Battery ADC reading and SoC calculation.
 *
 * @note    ADC1 Channel 4 (PA4) initialized manually here
 *          (not via CubeMX) to avoid modifying main.cpp.
 *          Polling mode — single conversion per update().
 *          Estimated Flash: ~0.8 KB, RAM: ~280 bytes (ADC handle).
 * ================================================================
 */
#include "battery_monitor.hpp"
#include "runtime_config.hpp"
#include "debug_uart.hpp"

// ================================================================
// Constructor
// ================================================================
BatteryMonitor::BatteryMonitor() {
    std::memset(&m_hadc, 0, sizeof(m_hadc));
}

// ================================================================
// Init ADC
// ================================================================
bool BatteryMonitor::init() {
    // Enable ADC1 clock
    __HAL_RCC_ADC1_CLK_ENABLE();

    // Configure PA4 as analog input
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef gpio{};
    gpio.Pin  = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    // Configure ADC1
    m_hadc.Instance                   = ADC1;
    m_hadc.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;
    m_hadc.Init.Resolution            = ADC_RESOLUTION_12B;
    m_hadc.Init.ScanConvMode          = DISABLE;
    m_hadc.Init.ContinuousConvMode    = DISABLE;
    m_hadc.Init.DiscontinuousConvMode = DISABLE;
    m_hadc.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    m_hadc.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    m_hadc.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    m_hadc.Init.NbrOfConversion       = 1;
    m_hadc.Init.DMAContinuousRequests = DISABLE;
    m_hadc.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;

    if (HAL_ADC_Init(&m_hadc) != HAL_OK) {
        DBG.error("BatMon: ADC init failed");
        return false;
    }

    m_initialized = true;
    DBG.info("BatMon: ADC1 CH4 (PA4) initialized");
    return true;
}

// ================================================================
// ADC to voltage conversion
// ================================================================
float BatteryMonitor::adcToVoltage(uint16_t raw) const {
    float adcVoltage = (static_cast<float>(raw) / static_cast<float>(ADC_MAX)) * VREF;
    return adcVoltage * VDIV_RATIO;
}

// ================================================================
// Voltage to SoC
// ================================================================
uint8_t BatteryMonitor::voltageToPercent(float voltage) const {
    if (voltage <= VBAT_MIN) return 0;
    if (voltage >= VBAT_MAX) return 100;

    float ratio = (voltage - VBAT_MIN) / (VBAT_MAX - VBAT_MIN);
    return static_cast<uint8_t>(ratio * 100.0f);
}

// ================================================================
// Update (single ADC conversion)
// ================================================================
void BatteryMonitor::update() {
    if (!m_initialized) return;

    // Configure channel
    ADC_ChannelConfTypeDef sConfig{};
    sConfig.Channel      = ADC_CHANNEL_4;
    sConfig.Rank         = 1;
    sConfig.SamplingTime = ADC_SAMPLETIME_84CYCLES;
    sConfig.Offset       = 0;

    if (HAL_ADC_ConfigChannel(&m_hadc, &sConfig) != HAL_OK) {
        DBG.error("BatMon: channel config failed");
        return;
    }

    // Start conversion
    HAL_ADC_Start(&m_hadc);

    if (HAL_ADC_PollForConversion(&m_hadc, 100) == HAL_OK) {
        m_rawAdc  = (uint16_t)HAL_ADC_GetValue(&m_hadc);
        m_voltage = adcToVoltage(m_rawAdc);
        m_percent = voltageToPercent(m_voltage);
    }

    HAL_ADC_Stop(&m_hadc);
}

// ================================================================
// Low battery check
// ================================================================
bool BatteryMonitor::isLow() const {
    return m_percent < Cfg().battery_low_pct;
}

// Estimated Flash: ~0.8 KB  |  RAM: ~280 bytes (ADC handle)
