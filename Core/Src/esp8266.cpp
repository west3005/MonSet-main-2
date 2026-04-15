/**
 * ================================================================
 * @file    esp8266.cpp
 * @brief   ESP8266 WiFi AT command driver implementation.
 *
 * @note    AT command set: ESP8266 NonOS AT 1.7.x / ESP-AT 2.x.
 *          USART6 shared with debug mirror — mode switch needed.
 *          Estimated Flash: ~2.5 KB, RAM: ~600 bytes.
 * ================================================================
 */
#include "esp8266.hpp"
#include "board_pins.hpp"
#include "debug_uart.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>

extern DebugUart DBG;

// ================================================================
// Constructor
// ================================================================
ESP8266::ESP8266(UART_HandleTypeDef* uart)
    : m_uart(uart)
{
}

// ================================================================
// Mode switching — USART6 is shared
// ================================================================
void ESP8266::enterEspMode() {
    if (m_espMode) return;
    // Disable debug mirror to free USART6
    DBG.setMirror(nullptr);
    m_espMode = true;
    DBG.info("ESP8266: entered ESP mode (USART6)");
}

void ESP8266::exitEspMode() {
    if (!m_espMode) return;
    // Re-enable debug mirror
    DBG.setMirror(m_uart);
    m_espMode = false;
    DBG.info("ESP8266: exited ESP mode (debug mirror restored)");
}

// ================================================================
// GPIO control
// ================================================================
void ESP8266::enable() {
    HAL_GPIO_WritePin(PIN_ESP_EN_PORT, PIN_ESP_EN_PIN, GPIO_PIN_SET);
}

void ESP8266::disable() {
    HAL_GPIO_WritePin(PIN_ESP_EN_PORT, PIN_ESP_EN_PIN, GPIO_PIN_RESET);
    m_initialized = false;
}

void ESP8266::hardReset() {
    HAL_GPIO_WritePin(PIN_ESP_RST_PORT, PIN_ESP_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(PIN_ESP_RST_PORT, PIN_ESP_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(2000);
    IWDG->KR = 0xAAAA;
}

// ================================================================
// Low-level UART I/O
// ================================================================
void ESP8266::sendRaw(const uint8_t* data, uint16_t len) {
    HAL_UART_Transmit(m_uart, (uint8_t*)data, len, 1000);
}

uint16_t ESP8266::readUntil(char* buf, uint16_t bsize, const char* expected,
                              uint32_t timeout) {
    uint16_t pos = 0;
    uint32_t t0 = HAL_GetTick();

    while ((HAL_GetTick() - t0) < timeout && pos < (bsize - 1)) {
        uint8_t c;
        if (HAL_UART_Receive(m_uart, &c, 1, 10) == HAL_OK) {
            buf[pos++] = (char)c;
            buf[pos] = 0;

            if (expected && std::strstr(buf, expected)) {
                return pos;
            }
        }
        IWDG->KR = 0xAAAA;
    }
    buf[pos] = 0;
    return pos;
}

bool ESP8266::sendCmd(const char* cmd, const char* expect, uint32_t timeout) {
    char cmdBuf[256];
    int n = std::snprintf(cmdBuf, sizeof(cmdBuf), "%s\r\n", cmd);
    sendRaw((const uint8_t*)cmdBuf, (uint16_t)n);

    char resp[512];
    uint16_t len = readUntil(resp, sizeof(resp), expect, timeout);

    if (len > 0 && std::strstr(resp, expect)) {
        return true;
    }

    return false;
}

bool ESP8266::sendCmdResp(const char* cmd, char* resp, uint16_t rsize, uint32_t timeout) {
    char cmdBuf[256];
    int n = std::snprintf(cmdBuf, sizeof(cmdBuf), "%s\r\n", cmd);
    sendRaw((const uint8_t*)cmdBuf, (uint16_t)n);

    uint16_t len = readUntil(resp, rsize, "OK", timeout);
    return (len > 0);
}

// ================================================================
// Init
// ================================================================
bool ESP8266::init() {
    if (!m_espMode) {
        DBG.error("ESP8266: not in ESP mode, call enterEspMode() first");
        return false;
    }

    enable();
    HAL_Delay(500);
    hardReset();

    // Disable echo
    sendCmd("ATE0", "OK", CMD_TIMEOUT_MS);

    // Test AT
    bool ok = false;
    for (int i = 0; i < 3; i++) {
        if (sendCmd("AT", "OK", CMD_TIMEOUT_MS)) {
            ok = true;
            break;
        }
        HAL_Delay(500);
        IWDG->KR = 0xAAAA;
    }

    if (!ok) {
        DBG.error("ESP8266: AT test failed");
        return false;
    }

    // Set station mode by default
    sendCmd("AT+CWMODE=1", "OK", CMD_TIMEOUT_MS);

    // Disable auto-connect
    sendCmd("AT+CWAUTOCONN=0", "OK", CMD_TIMEOUT_MS);

    m_initialized = true;
    DBG.info("ESP8266: initialized");
    return true;
}

// ================================================================
// Connect to WiFi (station mode)
// ================================================================
bool ESP8266::connectWifi(const char* ssid, const char* pass) {
    if (!m_initialized) return false;

    sendCmd("AT+CWMODE=1", "OK", CMD_TIMEOUT_MS);

    char cmd[160];
    std::snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, pass);

    if (!sendCmd(cmd, "WIFI GOT IP", CONNECT_TIMEOUT_MS)) {
        // Try alternative success marker
        char resp[256];
        uint16_t len = readUntil(resp, sizeof(resp), "OK", 5000);
        if (len == 0 || !std::strstr(resp, "OK")) {
            DBG.error("ESP8266: WiFi connect failed");
            return false;
        }
    }

    DBG.info("ESP8266: connected to %s", ssid);
    return true;
}

// ================================================================
// SoftAP
// ================================================================
bool ESP8266::startAP(const char* ssid, const char* pass) {
    if (!m_initialized) return false;

    // Set AP + Station mode
    sendCmd("AT+CWMODE=3", "OK", CMD_TIMEOUT_MS);

    char cmd[160];
    if (pass && pass[0]) {
        std::snprintf(cmd, sizeof(cmd), "AT+CWSAP=\"%s\",\"%s\",1,3", ssid, pass);
    } else {
        std::snprintf(cmd, sizeof(cmd), "AT+CWSAP=\"%s\",\"\",1,0", ssid);
    }

    if (!sendCmd(cmd, "OK", CMD_TIMEOUT_MS)) {
        DBG.error("ESP8266: start AP failed");
        return false;
    }

    // Enable DHCP for AP
    sendCmd("AT+CWDHCP=0,1", "OK", CMD_TIMEOUT_MS);

    m_apMode = true;
    DBG.info("ESP8266: AP started SSID=%s", ssid);
    return true;
}

void ESP8266::stopAP() {
    sendCmd("AT+CWMODE=1", "OK", CMD_TIMEOUT_MS);
    m_apMode = false;
}

// ================================================================
// Connection status
// ================================================================
bool ESP8266::isConnected() {
    char resp[128];
    sendCmdResp("AT+CIPSTATUS", resp, sizeof(resp), CMD_TIMEOUT_MS);
    // STATUS:2 = got IP, STATUS:3 = connected, STATUS:5 = disconnected
    return (std::strstr(resp, "STATUS:2") || std::strstr(resp, "STATUS:3"));
}

bool ESP8266::getIP(char* out, size_t outSz) {
    char resp[256];
    sendCmdResp("AT+CIFSR", resp, sizeof(resp), CMD_TIMEOUT_MS);

    // Parse STAIP or APIP
    const char* p = std::strstr(resp, "STAIP,\"");
    if (!p) p = std::strstr(resp, "APIP,\"");
    if (!p) return false;

    p = std::strchr(p, '"');
    if (!p) return false;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < outSz - 1) out[i++] = *p++;
    out[i] = 0;
    return (i > 0);
}

// ================================================================
// TCP connection
// ================================================================
bool ESP8266::tcpConnect(const char* host, uint16_t port) {
    // Single connection mode
    sendCmd("AT+CIPMUX=0", "OK", CMD_TIMEOUT_MS);

    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", host, (unsigned)port);

    if (!sendCmd(cmd, "CONNECT", CONNECT_TIMEOUT_MS)) {
        DBG.error("ESP8266: TCP connect failed");
        return false;
    }

    return true;
}

bool ESP8266::sendData(const uint8_t* data, uint16_t len) {
    char cmd[32];
    std::snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", (unsigned)len);

    if (!sendCmd(cmd, ">", 5000)) {
        DBG.error("ESP8266: CIPSEND failed");
        return false;
    }

    sendRaw(data, len);

    char resp[128];
    uint16_t rLen = readUntil(resp, sizeof(resp), "SEND OK", 10000);
    return (rLen > 0 && std::strstr(resp, "SEND OK"));
}

bool ESP8266::sendString(const char* str) {
    return sendData((const uint8_t*)str, (uint16_t)std::strlen(str));
}

void ESP8266::tcpClose() {
    sendCmd("AT+CIPCLOSE", "OK", CMD_TIMEOUT_MS);
}

// ================================================================
// TCP server
// ================================================================
bool ESP8266::startServer(uint16_t port) {
    // Enable multi-connection mode for server
    sendCmd("AT+CIPMUX=1", "OK", CMD_TIMEOUT_MS);

    char cmd[32];
    std::snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=1,%u", (unsigned)port);

    if (!sendCmd(cmd, "OK", CMD_TIMEOUT_MS)) {
        DBG.error("ESP8266: start server failed");
        return false;
    }

    DBG.info("ESP8266: server started on port %u", (unsigned)port);
    return true;
}

void ESP8266::stopServer() {
    sendCmd("AT+CIPSERVER=0", "OK", CMD_TIMEOUT_MS);
}

// ================================================================
// Receive data
// ================================================================
uint16_t ESP8266::receiveData(uint8_t* buf, uint16_t bufSize, uint32_t timeout) {
    char resp[512];
    uint16_t len = readUntil(resp, sizeof(resp), nullptr, timeout);
    if (len == 0) return 0;

    // Look for +IPD,<len>: prefix
    const char* ipd = std::strstr(resp, "+IPD,");
    if (!ipd) return 0;

    ipd += 5;
    uint16_t dataLen = (uint16_t)std::strtoul(ipd, nullptr, 10);

    const char* colon = std::strchr(ipd, ':');
    if (!colon) return 0;
    colon++;

    uint16_t available = (uint16_t)(len - (uint16_t)(colon - resp));
    if (available > dataLen) available = dataLen;
    if (available > bufSize) available = bufSize;

    std::memcpy(buf, colon, available);
    return available;
}

// ================================================================
// DNS server (for captive portal)
// ================================================================
bool ESP8266::setDnsServer(const char* ip) {
    // Not all ESP8266 firmware supports this natively.
    // Captive portal DNS is handled at application level.
    (void)ip;
    return true;
}

// Estimated Flash: ~2.5 KB  |  RAM: ~600 bytes
