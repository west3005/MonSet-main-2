/**
 * @file    modbus_tcp_master.cpp
 * @brief   Modbus TCP Master implementation — polls slave devices over W5500 TCP.
 *
 * Modbus TCP ADU layout (request, FC 03/04):
 *   [0..1]  Transaction ID  (big-endian)
 *   [2..3]  Protocol ID     = 0x0000
 *   [4..5]  Length          = 0x0006  (UnitID + FC + 4 data bytes)
 *   [6]     Unit ID
 *   [7]     Function Code   (0x03 or 0x04)
 *   [8..9]  Start Register  (big-endian)
 *   [10..11] Register Count (big-endian)
 *
 * Response ADU (success):
 *   [0..1]  Transaction ID  (echo)
 *   [2..3]  Protocol ID     (0x0000)
 *   [4..5]  Length          (big-endian, remaining byte count)
 *   [6]     Unit ID
 *   [7]     Function Code
 *   [8]     Byte Count      (= regCount * 2)
 *   [9..]   Register data   (big-endian, 2 bytes per register)
 */

#include "modbus_tcp_master.hpp"
#include "debug_uart.hpp"
#include "stm32f4xx_hal.h"
#include <cstring>
#include <cstdio>

extern "C" {
#include "socket.h"
}

// ---------------------------------------------------------------------------
// Internal constants
// ---------------------------------------------------------------------------

/// Minimum number of bytes in a valid Modbus TCP response (MBAP + FC + ByteCount)
static constexpr uint16_t MODBUS_RESP_MIN_LEN = 9U;

/// Maximum response buffer size: 9-byte header + up to 125 registers × 2 bytes
static constexpr uint16_t MODBUS_RESP_BUF_SIZE = 260U;

/// Modbus exception response: FC has bit 7 set
static constexpr uint8_t MODBUS_EXCEPTION_BIT = 0x80U;

/// Connect poll interval (ms)
static constexpr uint32_t CONNECT_POLL_MS = 5U;

// ---------------------------------------------------------------------------
// parseIpStr
// ---------------------------------------------------------------------------

bool ModbusTcpMaster::parseIpStr(const char* s, uint8_t ip[4])
{
    int a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        return false;
    }
    // Range-check each octet
    if (a < 0 || a > 255 || b < 0 || b > 255 ||
        c < 0 || c > 255 || d < 0 || d > 255) {
        return false;
    }
    ip[0] = static_cast<uint8_t>(a);
    ip[1] = static_cast<uint8_t>(b);
    ip[2] = static_cast<uint8_t>(c);
    ip[3] = static_cast<uint8_t>(d);
    return true;
}

// ---------------------------------------------------------------------------
// buildRequest
// ---------------------------------------------------------------------------

uint8_t ModbusTcpMaster::buildRequest(uint8_t* buf, uint16_t tid, uint8_t unitId,
                                       uint8_t fc, uint16_t startReg, uint8_t regCount)
{
    // MBAP Header
    buf[0] = static_cast<uint8_t>(tid >> 8);    // Transaction ID high
    buf[1] = static_cast<uint8_t>(tid & 0xFF);  // Transaction ID low
    buf[2] = 0x00;                               // Protocol ID high
    buf[3] = 0x00;                               // Protocol ID low
    buf[4] = 0x00;                               // Length high (PDU = 6 bytes)
    buf[5] = 0x06;                               // Length low
    // PDU
    buf[6] = unitId;                             // Unit ID
    buf[7] = fc;                                 // Function Code
    buf[8] = static_cast<uint8_t>(startReg >> 8);   // Start register high
    buf[9] = static_cast<uint8_t>(startReg & 0xFF); // Start register low
    buf[10] = 0x00;                              // Register count high
    buf[11] = regCount;                          // Register count low

    return 12U; // Fixed ADU length for FC 03/04
}

// ---------------------------------------------------------------------------
// waitRecv
// ---------------------------------------------------------------------------

int32_t ModbusTcpMaster::waitRecv(uint8_t sn, uint32_t timeoutMs)
{
    const uint32_t start = HAL_GetTick();
    while (static_cast<uint32_t>(HAL_GetTick() - start) < timeoutMs) {
        // Check socket is still established before querying RX size
        const uint8_t sr = getSn_SR(sn);
        if (sr != SOCK_ESTABLISHED && sr != SOCK_CLOSE_WAIT) {
            // Connection dropped by remote host
            return 0;
        }
        const uint16_t avail = getSn_RX_RSR(sn);
        if (avail > 0U) {
            return static_cast<int32_t>(avail);
        }
        HAL_Delay(CONNECT_POLL_MS);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// parseRegisters
// ---------------------------------------------------------------------------

float ModbusTcpMaster::parseRegisters(const uint8_t* regs, uint8_t dtype)
{
    switch (dtype) {
        case 0: {
            // INT16 big-endian — single register
            const int16_t val = static_cast<int16_t>(
                (static_cast<uint16_t>(regs[0]) << 8) | static_cast<uint16_t>(regs[1])
            );
            return static_cast<float>(val);
        }
        case 1: {
            // UINT16 big-endian — single register
            const uint16_t val =
                (static_cast<uint16_t>(regs[0]) << 8) | static_cast<uint16_t>(regs[1]);
            return static_cast<float>(val);
        }
        case 2: {
            // INT32 big-endian — two registers, high word first
            const int32_t val = static_cast<int32_t>(
                (static_cast<uint32_t>(regs[0]) << 24) |
                (static_cast<uint32_t>(regs[1]) << 16) |
                (static_cast<uint32_t>(regs[2]) << 8)  |
                 static_cast<uint32_t>(regs[3])
            );
            return static_cast<float>(val);
        }
        case 3: {
            // UINT32 big-endian — two registers, high word first
            const uint32_t val =
                (static_cast<uint32_t>(regs[0]) << 24) |
                (static_cast<uint32_t>(regs[1]) << 16) |
                (static_cast<uint32_t>(regs[2]) << 8)  |
                 static_cast<uint32_t>(regs[3]);
            return static_cast<float>(val);
        }
        case 4: {
            // FLOAT32 big-endian — two registers, high word first
            // Reconstruct the 32-bit pattern, then reinterpret as float via memcpy
            const uint32_t raw =
                (static_cast<uint32_t>(regs[0]) << 24) |
                (static_cast<uint32_t>(regs[1]) << 16) |
                (static_cast<uint32_t>(regs[2]) << 8)  |
                 static_cast<uint32_t>(regs[3]);
            float f = 0.0f;
            std::memcpy(&f, &raw, sizeof(f));
            return f;
        }
        default:
            // Unknown data type — return 0
            return 0.0f;
    }
}

// ---------------------------------------------------------------------------
// readDevice
// ---------------------------------------------------------------------------

bool ModbusTcpMaster::readDevice(const ModbusTcpDeviceCfg& dev, float& result)
{
    const uint8_t sn = dev.w5500_socket;

    // ------------------------------------------------------------------
    // 1. Open TCP socket
    // ------------------------------------------------------------------
    if (socket(sn, Sn_MR_TCP, 0, 0) != static_cast<int8_t>(sn)) {
        DBG.error("TCP_MASTER: socket() failed for %s", dev.name);
        return false;
    }

    // ------------------------------------------------------------------
    // 2. Parse IP string → uint8_t[4]
    // ------------------------------------------------------------------
    uint8_t ip[4] = {0, 0, 0, 0};
    if (!parseIpStr(dev.ip, ip)) {
        DBG.error("TCP_MASTER: bad IP \"%s\" for %s", dev.ip, dev.name);
        close(sn);
        return false;
    }

    // ------------------------------------------------------------------
    // 3. Initiate TCP connect
    // ------------------------------------------------------------------
    // ioLibrary connect() is non-blocking; it initiates the SYN handshake.
    // Cast away const on ip — ioLibrary API takes uint8_t* (not const).
    if (connect(sn, ip, dev.port) != SOCK_OK) {
        DBG.error("TCP_MASTER: connect() failed for %s", dev.name);
        close(sn);
        return false;
    }

    // Wait for ESTABLISHED with connect_timeout_ms
    {
        const uint32_t deadline = HAL_GetTick() + dev.connect_timeout_ms;
        bool connected = false;
        while (static_cast<int32_t>(deadline - HAL_GetTick()) > 0) {
            const uint8_t sr = getSn_SR(sn);
            if (sr == SOCK_ESTABLISHED) {
                connected = true;
                break;
            }
            if (sr == SOCK_CLOSED || sr == SOCK_FIN_WAIT) {
                // Connection refused or reset
                break;
            }
            HAL_Delay(CONNECT_POLL_MS);
        }
        if (!connected) {
            DBG.warn("TCP_MASTER: connect timeout for %s", dev.name);
            close(sn);
            return false;
        }
    }

    // ------------------------------------------------------------------
    // 4. Build Modbus TCP request
    // ------------------------------------------------------------------
    uint8_t req[12];
    const uint8_t reqLen = buildRequest(req,
                                         ++m_transactionId,
                                         dev.unit_id,
                                         dev.func_code,
                                         dev.reg_start,
                                         dev.reg_count);

    // ------------------------------------------------------------------
    // 5. Send request
    // ------------------------------------------------------------------
    const int32_t sent = send(sn, req, reqLen);
    if (sent != static_cast<int32_t>(reqLen)) {
        DBG.error("TCP_MASTER: send() failed for %s (sent=%ld)", dev.name, sent);
        close(sn);
        return false;
    }

    // ------------------------------------------------------------------
    // 6. Wait for response data
    // ------------------------------------------------------------------
    const int32_t available = waitRecv(sn, dev.poll_timeout_ms);
    if (available <= 0) {
        DBG.warn("TCP_MASTER: recv timeout for %s", dev.name);
        close(sn);
        return false;
    }

    // Clamp to buffer size
    const uint16_t recvLen = (static_cast<uint16_t>(available) < MODBUS_RESP_BUF_SIZE)
                             ? static_cast<uint16_t>(available)
                             : MODBUS_RESP_BUF_SIZE;

    uint8_t resp[MODBUS_RESP_BUF_SIZE];
    std::memset(resp, 0, sizeof(resp));

    const int32_t rcvd = recv(sn, resp, recvLen);
    if (rcvd < static_cast<int32_t>(MODBUS_RESP_MIN_LEN)) {
        DBG.warn("TCP_MASTER: short response (%ld bytes) for %s", rcvd, dev.name);
        close(sn);
        return false;
    }

    // ------------------------------------------------------------------
    // 7. Validate MBAP and PDU
    // ------------------------------------------------------------------

    // 7a. Transaction ID echo check
    const uint16_t respTid =
        (static_cast<uint16_t>(resp[0]) << 8) | static_cast<uint16_t>(resp[1]);
    if (respTid != m_transactionId) {
        DBG.warn("TCP_MASTER: TID mismatch (got %u, want %u) for %s",
                 respTid, m_transactionId, dev.name);
        close(sn);
        return false;
    }

    // 7b. Protocol ID must be 0x0000
    if (resp[2] != 0x00 || resp[3] != 0x00) {
        DBG.warn("TCP_MASTER: bad protocol ID for %s", dev.name);
        close(sn);
        return false;
    }

    // 7c. Check for Modbus exception response (FC | 0x80)
    const uint8_t respFc = resp[7];
    if (respFc & MODBUS_EXCEPTION_BIT) {
        const uint8_t exCode = (rcvd >= 9) ? resp[8] : 0U;
        DBG.warn("TCP_MASTER: Modbus exception FC=0x%02X code=%u for %s",
                 respFc, exCode, dev.name);
        close(sn);
        return false;
    }

    // 7d. Function code must match request
    if (respFc != dev.func_code) {
        DBG.warn("TCP_MASTER: FC mismatch (got 0x%02X, want 0x%02X) for %s",
                 respFc, dev.func_code, dev.name);
        close(sn);
        return false;
    }

    // 7e. Byte count sanity check
    const uint8_t byteCount = resp[8];
    const uint8_t expectedBytes = static_cast<uint8_t>(dev.reg_count * 2U);
    if (byteCount != expectedBytes) {
        DBG.warn("TCP_MASTER: byte count %u != expected %u for %s",
                 byteCount, expectedBytes, dev.name);
        close(sn);
        return false;
    }

    // 7f. Ensure we actually received that many register bytes
    if (rcvd < static_cast<int32_t>(9 + byteCount)) {
        DBG.warn("TCP_MASTER: truncated payload for %s", dev.name);
        close(sn);
        return false;
    }

    // ------------------------------------------------------------------
    // 8. Parse register data and apply scale/offset
    // ------------------------------------------------------------------
    const float raw = parseRegisters(&resp[9], dev.data_type);
    result = raw * dev.scale + dev.offset;

    // ------------------------------------------------------------------
    // 9. Close socket
    // ------------------------------------------------------------------
    close(sn);
    return true;
}

// ---------------------------------------------------------------------------
// pollAll
// ---------------------------------------------------------------------------

void ModbusTcpMaster::pollAll(const ModbusTcpMasterConfig& cfg,
                               float* readings,
                               uint8_t readingsMax)
{
    for (uint8_t i = 0U; i < cfg.device_count; ++i) {
        const ModbusTcpDeviceCfg& dev = cfg.devices[i];

        if (!dev.enabled) {
            continue;
        }

        float val = 0.0f;
        const bool ok = readDevice(dev, val);

        if (ok) {
            if (dev.channel_idx < readingsMax) {
                readings[dev.channel_idx] = val;
            }
            DBG.info("TCP_MASTER: %s -> %.3f %s", dev.name, val, dev.unit);
        } else {
            DBG.warn("TCP_MASTER: %s FAIL", dev.name);
        }
    }
}
