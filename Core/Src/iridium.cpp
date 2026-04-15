/**
 * ================================================================
 * @file    iridium.cpp
 * @brief   Iridium 9602/NR9602G SBD driver implementation.
 *
 * @note    AT command sequence for SBD send:
 *          1. AT (test)
 *          2. ATE0 (disable echo)
 *          3. AT+CSQ (check signal)
 *          4. AT+SBDWB=<len> (write binary to MO buffer)
 *          5. <binary data + 2-byte checksum>
 *          6. AT+SBDIX (initiate SBD session)
 *          7. Parse +SBDIX response for MO status
 *
 *          Estimated Flash: ~2.5 KB, RAM: ~400 bytes.
 * ================================================================
 */
#include "iridium.hpp"
#include "board_pins.hpp"
#include "debug_uart.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ================================================================
// Constructor
// ================================================================
Iridium::Iridium(UART_HandleTypeDef* uart)
    : m_uart(uart)
{
}

// ================================================================
// GPIO control
// ================================================================
void Iridium::powerOn() {
    HAL_GPIO_WritePin(PIN_EDGE_ON_PORT, PIN_EDGE_ON_PIN, GPIO_PIN_SET);
    DBG.info("Iridium: power ON");
    HAL_Delay(3000); // Allow modem to boot
    IWDG->KR = 0xAAAA;
}

void Iridium::powerOff() {
    HAL_GPIO_WritePin(PIN_EDGE_ON_PORT, PIN_EDGE_ON_PIN, GPIO_PIN_RESET);
    m_initialized = false;
    DBG.info("Iridium: power OFF");
}

bool Iridium::isPowered() const {
    return HAL_GPIO_ReadPin(PIN_EDGE_PWR_DET_PORT, PIN_EDGE_PWR_DET_PIN) == GPIO_PIN_SET;
}

bool Iridium::isNetworkAvailable() const {
    return HAL_GPIO_ReadPin(PIN_EDGE_NET_AVAIL_PORT, PIN_EDGE_NET_AVAIL_PIN) == GPIO_PIN_SET;
}

// ================================================================
// Low-level UART I/O
// ================================================================
void Iridium::sendRaw(const uint8_t* data, uint16_t len) {
    HAL_UART_Transmit(m_uart, (uint8_t*)data, len, 1000);
}

uint16_t Iridium::readResponse(char* buf, uint16_t bsize, uint32_t timeout) {
    uint16_t pos = 0;
    uint32_t t0 = HAL_GetTick();

    while ((HAL_GetTick() - t0) < timeout && pos < (bsize - 1)) {
        uint8_t c;
        if (HAL_UART_Receive(m_uart, &c, 1, 10) == HAL_OK) {
            buf[pos++] = (char)c;
        }
        IWDG->KR = 0xAAAA;
    }

    buf[pos] = 0;
    return pos;
}

bool Iridium::sendCmd(const char* cmd, char* resp, uint16_t rsize, uint32_t timeout) {
    // Send command with CR
    char cmdBuf[128];
    int n = std::snprintf(cmdBuf, sizeof(cmdBuf), "%s\r", cmd);
    sendRaw((const uint8_t*)cmdBuf, (uint16_t)n);

    // Read response
    uint16_t len = readResponse(resp, rsize, timeout);
    if (len == 0) return false;

    DBG.info("Iridium: cmd='%s' resp_len=%u", cmd, (unsigned)len);
    return true;
}

bool Iridium::waitFor(const char* expected, uint32_t timeout) {
    char buf[256];
    uint16_t pos = 0;
    uint32_t t0 = HAL_GetTick();

    while ((HAL_GetTick() - t0) < timeout && pos < sizeof(buf) - 1) {
        uint8_t c;
        if (HAL_UART_Receive(m_uart, &c, 1, 10) == HAL_OK) {
            buf[pos++] = (char)c;
            buf[pos] = 0;
            if (std::strstr(buf, expected)) return true;
        }
        IWDG->KR = 0xAAAA;
    }
    return false;
}

// ================================================================
// Init
// ================================================================
bool Iridium::init() {
    if (!isPowered()) {
        DBG.error("Iridium: not powered");
        return false;
    }

    char resp[128];

    // Test AT
    for (int i = 0; i < 3; i++) {
        if (sendCmd("AT", resp, sizeof(resp), AT_TIMEOUT_MS)) {
            if (std::strstr(resp, "OK")) break;
        }
        HAL_Delay(500);
        IWDG->KR = 0xAAAA;
    }

    // Disable echo
    sendCmd("ATE0", resp, sizeof(resp), AT_TIMEOUT_MS);

    // Check modem info
    sendCmd("AT+CGMI", resp, sizeof(resp), AT_TIMEOUT_MS);
    DBG.info("Iridium: manufacturer: %s", resp);

    // Clear MO buffer
    sendCmd("AT+SBDD0", resp, sizeof(resp), AT_TIMEOUT_MS);

    m_initialized = true;
    DBG.info("Iridium: initialized");
    return true;
}

// ================================================================
// Check signal quality
// ================================================================
int8_t Iridium::checkSignal() {
    char resp[64];
    if (!sendCmd("AT+CSQ", resp, sizeof(resp), AT_TIMEOUT_MS)) return -1;

    // Response: +CSQ:X where X is 0-5
    const char* p = std::strstr(resp, "+CSQ:");
    if (!p) return -1;
    p += 5;

    int8_t sig = (int8_t)std::strtol(p, nullptr, 10);
    DBG.info("Iridium: signal=%d/5", (int)sig);
    return sig;
}

// ================================================================
// Write binary to MO buffer
// ================================================================
bool Iridium::writeBinaryBuffer(const uint8_t* data, uint16_t len) {
    if (len > SBD_MAX_PAYLOAD) return false;

    // AT+SBDWB=<len>
    char cmd[32];
    std::snprintf(cmd, sizeof(cmd), "AT+SBDWB=%u", (unsigned)len);

    char resp[64];
    sendCmd(cmd, resp, sizeof(resp), AT_TIMEOUT_MS);

    // Wait for READY prompt
    if (!std::strstr(resp, "READY")) {
        if (!waitFor("READY", 5000)) {
            DBG.error("Iridium: no READY prompt");
            return false;
        }
    }

    // Send binary data
    sendRaw(data, len);

    // Send 2-byte checksum (sum of all bytes, big-endian)
    uint16_t checksum = 0;
    for (uint16_t i = 0; i < len; i++) checksum += data[i];
    uint8_t csBytes[2] = {(uint8_t)(checksum >> 8), (uint8_t)(checksum & 0xFF)};
    sendRaw(csBytes, 2);

    // Read response — expect "0" (success) or "1" (timeout) or "2" (bad checksum)
    uint16_t rlen = readResponse(resp, sizeof(resp), AT_TIMEOUT_MS);
    if (rlen == 0) return false;

    // Parse status
    for (uint16_t i = 0; i < rlen; i++) {
        if (resp[i] == '0' && (i == 0 || resp[i-1] == '\n' || resp[i-1] == '\r')) {
            DBG.info("Iridium: SBDWB OK (%u bytes)", (unsigned)len);
            return true;
        }
    }

    DBG.error("Iridium: SBDWB failed: %s", resp);
    return false;
}

// ================================================================
// Initiate SBD session
// ================================================================
int8_t Iridium::initiateSbdSession() {
    char resp[128];
    if (!sendCmd("AT+SBDIX", resp, sizeof(resp), SBD_TIMEOUT_MS)) return -1;

    // Parse +SBDIX: <MO status>, <MOMSN>, <MT status>, <MTMSN>, <MT length>, <MT queued>
    const char* p = std::strstr(resp, "+SBDIX:");
    if (!p) {
        // May need to wait longer for response
        uint16_t extra = readResponse(resp, sizeof(resp), SBD_TIMEOUT_MS);
        p = std::strstr(resp, "+SBDIX:");
        if (!p) return -1;
    }

    p += 7; // skip "+SBDIX:"
    while (*p == ' ') p++;

    int moStatus = (int)std::strtol(p, nullptr, 10);
    DBG.info("Iridium: SBDIX MO status=%d", moStatus);

    return (int8_t)moStatus;
}

// ================================================================
// Send SBD message with retries
// ================================================================
IridiumStatus Iridium::sendSBD(const uint8_t* data, uint16_t len) {
    if (!m_initialized) return IridiumStatus::NotReady;
    if (len > SBD_MAX_PAYLOAD) return IridiumStatus::BufferFull;

    // Check signal
    int8_t sig = checkSignal();
    if (sig < 1) {
        DBG.warn("Iridium: weak signal (%d), trying anyway...", (int)sig);
    }

    // Write to MO buffer
    if (!writeBinaryBuffer(data, len)) {
        return IridiumStatus::SendFailed;
    }

    // Try SBD session with retries
    for (uint8_t attempt = 0; attempt < MAX_RETRIES; attempt++) {
        int8_t moStatus = initiateSbdSession();

        // MO status 0-4 means success
        if (moStatus >= 0 && moStatus <= 4) {
            DBG.info("Iridium: SBD sent OK (attempt %u, MO=%d)",
                     (unsigned)(attempt + 1), (int)moStatus);
            return IridiumStatus::Ok;
        }

        // Status 32 = no network — try again
        DBG.warn("Iridium: SBDIX attempt %u failed (MO=%d)",
                 (unsigned)(attempt + 1), (int)moStatus);

        if (attempt < MAX_RETRIES - 1) {
            HAL_Delay(10000); // Wait 10s before retry
            IWDG->KR = 0xAAAA;
        }
    }

    return IridiumStatus::SendFailed;
}

// ================================================================
// Pack sensor data into compact binary
// ================================================================
uint16_t Iridium::packSensorData(uint8_t* out, uint16_t outMax,
                                   const float* values, uint8_t count,
                                   uint32_t unixTs) {
    // Binary format:
    // [0]    = magic 0xAB
    // [1]    = version 1
    // [2]    = sensor count
    // [3-6]  = unix timestamp (big-endian)
    // [7..] = float values (4 bytes each, big-endian)

    uint16_t needed = 7 + count * 4;
    if (outMax < needed || needed > SBD_MAX_PAYLOAD) return 0;

    out[0] = 0xAB;
    out[1] = 0x01;
    out[2] = count;

    // Timestamp big-endian
    out[3] = (uint8_t)(unixTs >> 24);
    out[4] = (uint8_t)(unixTs >> 16);
    out[5] = (uint8_t)(unixTs >> 8);
    out[6] = (uint8_t)(unixTs);

    // Sensor values
    for (uint8_t i = 0; i < count; i++) {
        uint32_t raw;
        std::memcpy(&raw, &values[i], 4);
        uint16_t offset = 7 + i * 4;
        out[offset + 0] = (uint8_t)(raw >> 24);
        out[offset + 1] = (uint8_t)(raw >> 16);
        out[offset + 2] = (uint8_t)(raw >> 8);
        out[offset + 3] = (uint8_t)(raw);
    }

    return needed;
}

// Estimated Flash: ~2.5 KB  |  RAM: ~400 bytes
