/**
 * ================================================================
 * @file    iridium.hpp
 * @brief   Iridium 9602/NR9602G SBD transceiver driver.
 *
 * @note    Hardware: UART5 (PC12/PD2), GPIO control:
 *            EDGE_ON (PD3), EDGE_NET_AVAIL (PD4), EDGE_PWR_DET (PD5).
 *          SBD payload limit: 340 bytes (binary, not JSON).
 *          Compact binary encoding for sensor data.
 * ================================================================
 */
#pragma once

#include "stm32f4xx_hal.h"
#include <cstdint>

/// Iridium send result
enum class IridiumStatus : uint8_t {
    Ok           = 0,
    NoSignal     = 1,
    SendFailed   = 2,
    Timeout      = 3,
    NotReady     = 4,
    BufferFull   = 5
};

class Iridium {
public:
    /// @brief Construct with UART handle and GPIO pins
    Iridium(UART_HandleTypeDef* uart);

    /// @brief Initialize modem (AT test, disable echo, set SBD mode)
    bool init();

    /// @brief Power on the modem (EDGE_ON pin)
    void powerOn();

    /// @brief Power off the modem
    void powerOff();

    /// @brief Check if modem has power (EDGE_PWR_DET)
    bool isPowered() const;

    /// @brief Check if network is available (EDGE_NET_AVAIL)
    bool isNetworkAvailable() const;

    /// @brief Check signal quality (0-5, >=2 is good for SBD)
    /// @return signal bars 0-5, or -1 on error
    int8_t checkSignal();

    /// @brief Send binary SBD message (max 340 bytes)
    /// @param data  Binary payload
    /// @param len   Payload length (max 340)
    /// @return IridiumStatus
    IridiumStatus sendSBD(const uint8_t* data, uint16_t len);

    /// @brief Pack sensor data into compact binary format
    /// @param out     Output buffer (min 32 bytes recommended)
    /// @param outMax  Buffer size
    /// @param values  Array of float sensor values
    /// @param count   Number of values
    /// @param unixTs  Unix timestamp
    /// @return bytes written, or 0 on error
    static uint16_t packSensorData(uint8_t* out, uint16_t outMax,
                                    const float* values, uint8_t count,
                                    uint32_t unixTs);

private:
    UART_HandleTypeDef* m_uart;
    bool m_initialized = false;

    /// SBD maximum payload size
    static constexpr uint16_t SBD_MAX_PAYLOAD = 340;

    /// AT command timeout
    static constexpr uint32_t AT_TIMEOUT_MS = 5000;

    /// SBD session timeout (can take up to 60 seconds)
    static constexpr uint32_t SBD_TIMEOUT_MS = 90000;

    /// Max send retries
    static constexpr uint8_t MAX_RETRIES = 3;

    /// @brief Send AT command and wait for response
    bool sendCmd(const char* cmd, char* resp, uint16_t rsize, uint32_t timeout);

    /// @brief Send raw data to UART
    void sendRaw(const uint8_t* data, uint16_t len);

    /// @brief Read UART response
    uint16_t readResponse(char* buf, uint16_t bsize, uint32_t timeout);

    /// @brief Wait for specific string in response
    bool waitFor(const char* expected, uint32_t timeout);

    /// @brief Write binary data to SBD buffer (AT+SBDWB)
    bool writeBinaryBuffer(const uint8_t* data, uint16_t len);

    /// @brief Initiate SBD session (AT+SBDIX) and parse response
    /// @return MO status code (0-4 = success, >4 = failure)
    int8_t initiateSbdSession();
};
