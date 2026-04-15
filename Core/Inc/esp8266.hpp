/**
 * ================================================================
 * @file    esp8266.hpp
 * @brief   ESP8266 WiFi AT command driver on USART6.
 *
 * @note    USART6 (PC6/PC7) is SHARED with debug mirror and
 *          cfg_uart_bridge. A mode switch is needed before using
 *          ESP8266 — call enterEspMode()/exitEspMode().
 *          GPIO control: EN (PE0), RST (PE1), GPIO0 (PE2).
 * ================================================================
 */
#pragma once

#include "stm32f4xx_hal.h"
#include <cstdint>

/// ESP8266 WiFi mode
enum class EspWifiMode : uint8_t {
    Station = 1,   ///< Connect to existing AP
    SoftAP  = 2,   ///< Act as access point
    Both    = 3    ///< Station + SoftAP
};

class ESP8266 {
public:
    ESP8266(UART_HandleTypeDef* uart);

    /// @brief Initialize ESP8266 (hardware reset, AT test, set mode)
    bool init();

    /// @brief Enter ESP mode (switches USART6 from debug mirror)
    void enterEspMode();

    /// @brief Exit ESP mode (returns USART6 to debug mirror)
    void exitEspMode();

    /// @brief Check if in ESP mode
    bool isEspMode() const { return m_espMode; }

    /// @brief Hardware reset via RST pin
    void hardReset();

    /// @brief Enable/disable ESP8266 via EN pin
    void enable();
    void disable();

    /// @brief Connect to WiFi AP (station mode)
    bool connectWifi(const char* ssid, const char* pass);

    /// @brief Start as SoftAP
    bool startAP(const char* ssid, const char* pass);

    /// @brief Stop SoftAP
    void stopAP();

    /// @brief Check if connected to WiFi
    bool isConnected();

    /// @brief Get assigned IP address as string
    bool getIP(char* out, size_t outSz);

    /// @brief Start TCP connection
    bool tcpConnect(const char* host, uint16_t port);

    /// @brief Send data over TCP
    bool sendData(const uint8_t* data, uint16_t len);

    /// @brief Send string over TCP
    bool sendString(const char* str);

    /// @brief Close TCP connection
    void tcpClose();

    /// @brief Start TCP server on given port
    bool startServer(uint16_t port);

    /// @brief Stop TCP server
    void stopServer();

    /// @brief Check for incoming data, read into buffer
    /// @return bytes read, or 0 if no data
    uint16_t receiveData(uint8_t* buf, uint16_t bufSize, uint32_t timeout);

    /// @brief Configure DNS server (for captive portal)
    bool setDnsServer(const char* ip);

private:
    UART_HandleTypeDef* m_uart;
    bool m_initialized = false;
    bool m_espMode     = false;
    bool m_apMode      = false;

    static constexpr uint32_t CMD_TIMEOUT_MS     = 5000;
    static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

    /// @brief Send AT command and wait for expected response
    bool sendCmd(const char* cmd, const char* expect, uint32_t timeout);

    /// @brief Send AT command and read response
    bool sendCmdResp(const char* cmd, char* resp, uint16_t rsize, uint32_t timeout);

    /// @brief Send raw bytes
    void sendRaw(const uint8_t* data, uint16_t len);

    /// @brief Read response until expected string or timeout
    uint16_t readUntil(char* buf, uint16_t bsize, const char* expected, uint32_t timeout);
};
