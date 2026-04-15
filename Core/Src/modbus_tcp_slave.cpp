/**
 * @file    modbus_tcp_slave.cpp
 * @brief   Modbus TCP Slave implementation — MonSet / W5500 / STM32F4.
 *
 * Supports FC02 (Read Discrete Inputs) and FC03 (Read Holding Registers).
 * No heap, no exceptions, no RTTI, no std::string.
 */
#include "modbus_tcp_slave.hpp"
#include "debug_uart.hpp"
#include "stm32f4xx_hal.h"
#include <cstring>
#include <cstdio>

extern "C" {
#include "socket.h"
}

// ============================================================
// Helper: min for uint16_t (avoid pulling in <algorithm>)
// ============================================================
static inline uint16_t u16min(uint16_t a, uint16_t b)
{
    return (a < b) ? a : b;
}

// ============================================================
// init()
// ============================================================
void ModbusTcpSlave::init(const ModbusTcpSlaveConfig& cfg)
{
    m_socket  = cfg.w5500_socket;
    m_running = false;

    socket(m_socket, Sn_MR_TCP, cfg.listen_port, 0);
    listen(m_socket);
    m_running = true;

    DBG.info("TCP_SLAVE: listening on port %u (socket %u)",
             cfg.listen_port, m_socket);
}

// ============================================================
// tick()
// ============================================================
void ModbusTcpSlave::tick(const ModbusTcpSlaveConfig& cfg,
                          const float* readings, uint8_t battPct,
                          float battV, uint8_t chanStatus)
{
    if (!m_running) {
        return;
    }

    uint8_t sr = getSn_SR(m_socket);

    switch (sr) {
    case SOCK_LISTEN:
        // Waiting for a client — nothing to do
        break;

    case SOCK_ESTABLISHED: {
        uint16_t avail = getSn_RX_RSR(m_socket);
        if (avail >= 8u) {
            // Read up to BUF_SIZE bytes
            uint16_t toRead = u16min(avail, BUF_SIZE);
            recv(m_socket, m_req, toRead);
            handleRequest(cfg, readings, battPct, battV, chanStatus);
        }

        // If client has closed the connection, re-open and listen
        uint8_t srAfter = getSn_SR(m_socket);
        if (srAfter == SOCK_CLOSE_WAIT ||
            srAfter == SOCK_CLOSED     ||
            srAfter == SOCK_FIN_WAIT   ||
            srAfter == SOCK_CLOSING    ||
            srAfter == SOCK_TIME_WAIT  ||
            srAfter == SOCK_LAST_ACK)
        {
            close(m_socket);
            socket(m_socket, Sn_MR_TCP, cfg.listen_port, 0);
            listen(m_socket);
            DBG.info("TCP_SLAVE: client disconnected, re-listening");
        }
        break;
    }

    case SOCK_CLOSE_WAIT:
    case SOCK_CLOSED:
        // Socket was reset externally; reopen
        close(m_socket);
        socket(m_socket, Sn_MR_TCP, cfg.listen_port, 0);
        listen(m_socket);
        DBG.info("TCP_SLAVE: socket reset, re-listening");
        break;

    default:
        // Transient states (SYN_RECV, FIN_WAIT, etc.) — wait
        break;
    }
}

// ============================================================
// handleRequest()
// ============================================================
void ModbusTcpSlave::handleRequest(const ModbusTcpSlaveConfig& cfg,
                                   const float* readings, uint8_t battPct,
                                   float battV, uint8_t chanStatus)
{
    // Minimum Modbus TCP ADU is 8 bytes:
    // MBAP (7 bytes) + FC (1 byte)
    // req[0..1] = Transaction ID
    // req[2..3] = Protocol ID (must be 0x0000)
    // req[4..5] = Length (bytes following, including unit ID)
    // req[6]    = Unit ID
    // req[7]    = Function Code
    // req[8..N] = PDU data

    uint8_t fc = m_req[7];

    uint16_t respLen = 0;

    if (fc == 0x03) {
        // FC03 — Read Holding Registers
        uint16_t startReg = static_cast<uint16_t>((m_req[8] << 8) | m_req[9]);
        uint16_t regCnt   = static_cast<uint16_t>((m_req[10] << 8) | m_req[11]);

        respLen = buildFC03Response(cfg, readings, battPct, battV, chanStatus,
                                    m_req, m_resp, BUF_SIZE);
        if (respLen > 0) {
            send(m_socket, m_resp, respLen);
            DBG.info("TCP_SLAVE: FC%02X regs[%u..%u] served",
                     fc, startReg,
                     static_cast<uint16_t>(startReg + (regCnt > 0 ? regCnt - 1u : 0u)));
        } else {
            sendException(m_socket, m_req, fc, 0x02); // Illegal Data Address
        }

    } else if (fc == 0x02) {
        // FC02 — Read Discrete Inputs
        uint16_t startBit = static_cast<uint16_t>((m_req[8] << 8) | m_req[9]);
        uint16_t bitCnt   = static_cast<uint16_t>((m_req[10] << 8) | m_req[11]);

        respLen = buildFC02Response(chanStatus, m_req, m_resp, BUF_SIZE);
        if (respLen > 0) {
            send(m_socket, m_resp, respLen);
            DBG.info("TCP_SLAVE: FC%02X bits[%u..%u] served",
                     fc, startBit,
                     static_cast<uint16_t>(startBit + (bitCnt > 0 ? bitCnt - 1u : 0u)));
        } else {
            sendException(m_socket, m_req, fc, 0x02); // Illegal Data Address
        }

    } else {
        // Unsupported function code
        sendException(m_socket, m_req, fc, 0x01); // Illegal Function
        DBG.warn("TCP_SLAVE: unsupported FC=0x%02X", fc);
    }
}

// ============================================================
// buildFC03Response()
// ============================================================
uint16_t ModbusTcpSlave::buildFC03Response(const ModbusTcpSlaveConfig& cfg,
                                            const float* readings,
                                            uint8_t battPct,
                                            float battV,
                                            uint8_t chanStatus,
                                            const uint8_t* req,
                                            uint8_t* resp,
                                            uint16_t respMax)
{
    uint16_t startReg = static_cast<uint16_t>((req[8] << 8) | req[9]);
    uint16_t regCnt   = static_cast<uint16_t>((req[10] << 8) | req[11]);

    if (regCnt == 0 || regCnt > 125u) {
        return 0; // Illegal Data Value
    }

    // PDU payload: FC(1) + ByteCount(1) + regCnt*2 bytes
    uint16_t dataBytes = static_cast<uint16_t>(regCnt * 2u);
    // Total response: MBAP(7) + PDU
    uint16_t totalLen = static_cast<uint16_t>(7u + 1u + 1u + dataBytes);

    if (totalLen > respMax) {
        return 0; // Buffer too small
    }

    // ---- MBAP header ----
    resp[0] = req[0];  // Transaction ID hi
    resp[1] = req[1];  // Transaction ID lo
    resp[2] = 0x00;    // Protocol ID hi
    resp[3] = 0x00;    // Protocol ID lo
    // Length field: number of bytes after this field = UnitID(1) + FC(1) + ByteCount(1) + data
    uint16_t mbapLen = static_cast<uint16_t>(1u + 1u + 1u + dataBytes);
    resp[4] = static_cast<uint8_t>(mbapLen >> 8);
    resp[5] = static_cast<uint8_t>(mbapLen & 0xFF);
    resp[6] = cfg.unit_id;   // Unit ID

    // ---- PDU ----
    resp[7] = 0x03;                              // FC03
    resp[8] = static_cast<uint8_t>(dataBytes);   // Byte count

    uint8_t* dataPtr = &resp[9];

    // Fill each requested register
    for (uint16_t i = 0; i < regCnt; /* increment inside */) {
        uint16_t addr = static_cast<uint16_t>(startReg + i);

        // Search the reg_map for this address
        bool found = false;
        for (uint8_t e = 0; e < cfg.reg_count; ++e) {
            const ModbusTcpSlaveRegEntry& entry = cfg.reg_map[e];

            // For FLOAT32 entries occupying 2 registers, check both addresses
            uint8_t entryRegs = (entry.data_type == 4u) ? 2u : 1u;

            if (addr >= entry.reg_addr &&
                addr < static_cast<uint16_t>(entry.reg_addr + entryRegs))
            {
                // Get the full value for this entry
                uint16_t outRegs[2] = {0, 0};
                uint8_t  nRegs = getRegValue(entry, readings, battPct, battV,
                                             chanStatus, outRegs);

                // Write the word(s) that fall within the requested range
                uint16_t offset = static_cast<uint16_t>(addr - entry.reg_addr);
                for (uint8_t r = offset; r < nRegs && i < regCnt; ++r, ++i) {
                    *dataPtr++ = static_cast<uint8_t>(outRegs[r] >> 8);
                    *dataPtr++ = static_cast<uint8_t>(outRegs[r] & 0xFF);
                }
                found = true;
                break;
            }
        }

        if (!found) {
            // Register not in map — return 0x0000
            *dataPtr++ = 0x00;
            *dataPtr++ = 0x00;
            ++i;
        }
    }

    return totalLen;
}

// ============================================================
// buildFC02Response()
// ============================================================
uint16_t ModbusTcpSlave::buildFC02Response(uint8_t chanStatus,
                                            const uint8_t* req,
                                            uint8_t* resp,
                                            uint16_t respMax)
{
    uint16_t startBit = static_cast<uint16_t>((req[8] << 8) | req[9]);
    uint16_t bitCnt   = static_cast<uint16_t>((req[10] << 8) | req[11]);

    if (bitCnt == 0 || bitCnt > 8u) {
        // We expose at most 8 channel status bits (bits 0..3 valid)
        return 0;
    }

    // Number of coil bytes = ceil(bitCnt / 8)
    uint8_t coilBytes = static_cast<uint8_t>((bitCnt + 7u) / 8u);

    // Total response: MBAP(7) + FC(1) + ByteCount(1) + coilBytes
    uint16_t totalLen = static_cast<uint16_t>(7u + 1u + 1u + coilBytes);
    if (totalLen > respMax) {
        return 0;
    }

    // ---- MBAP header ----
    resp[0] = req[0];  // Transaction ID hi
    resp[1] = req[1];  // Transaction ID lo
    resp[2] = 0x00;    // Protocol ID hi
    resp[3] = 0x00;    // Protocol ID lo
    uint16_t mbapLen = static_cast<uint16_t>(1u + 1u + 1u + coilBytes);
    resp[4] = static_cast<uint8_t>(mbapLen >> 8);
    resp[5] = static_cast<uint8_t>(mbapLen & 0xFF);
    resp[6] = req[6];  // Unit ID (echo from request)

    // ---- PDU ----
    resp[7] = 0x02;                            // FC02
    resp[8] = coilBytes;                       // Byte count

    // Pack requested bits from chanStatus, starting at startBit
    // Bits 0..3 of chanStatus = ETH, GSM, WIFI, IRIDIUM
    for (uint8_t b = 0; b < coilBytes; ++b) {
        uint8_t byteVal = 0;
        for (uint8_t bit = 0; bit < 8u; ++bit) {
            uint16_t globalBit = static_cast<uint16_t>(startBit + b * 8u + bit);
            if ((b * 8u + bit) >= bitCnt) break; // past requested count

            if (globalBit < 8u) {
                // Map bit to chanStatus
                if ((chanStatus >> globalBit) & 0x01u) {
                    byteVal |= static_cast<uint8_t>(1u << bit);
                }
            }
            // Bits >= 8 are always 0 (not defined)
        }
        resp[9 + b] = byteVal;
    }

    return totalLen;
}

// ============================================================
// getRegValue()
// ============================================================
uint8_t ModbusTcpSlave::getRegValue(const ModbusTcpSlaveRegEntry& entry,
                                     const float* readings,
                                     uint8_t battPct,
                                     float battV,
                                     uint8_t chanStatus,
                                     uint16_t* outRegs)
{
    float val = 0.0f;

    switch (entry.source) {
    case 0:
    case 1:
    case 2:
    case 3:
        // Sensor reading (float), scaled
        val = readings[entry.source] * entry.scale;
        break;
    case 4:
        // Battery percent (integer, no scale applied for UINT16 path)
        val = static_cast<float>(battPct);
        break;
    case 5:
        // Battery voltage (float), scaled
        val = battV * entry.scale;
        break;
    case 6:
        // Channel status bitmask (integer)
        val = static_cast<float>(chanStatus);
        break;
    default:
        val = 0.0f;
        break;
    }

    switch (entry.data_type) {
    case 0u: {
        // INT16 — 1 register
        auto i16 = static_cast<int16_t>(val);
        outRegs[0] = static_cast<uint16_t>(i16);
        return 1u;
    }
    case 1u: {
        // UINT16 — 1 register
        outRegs[0] = static_cast<uint16_t>(static_cast<uint32_t>(val));
        return 1u;
    }
    case 4u: {
        // FLOAT32 — 2 registers, big-endian word order
        // IEEE 754 float → uint32 → high word in outRegs[0], low word in outRegs[1]
        uint32_t u32 = 0u;
        static_assert(sizeof(float) == sizeof(uint32_t),
                      "float must be 32-bit on this platform");
        std::memcpy(&u32, &val, sizeof(u32));
        outRegs[0] = static_cast<uint16_t>(u32 >> 16);        // High word
        outRegs[1] = static_cast<uint16_t>(u32 & 0xFFFFu);    // Low word
        return 2u;
    }
    default:
        // Unknown type — return 0
        outRegs[0] = 0u;
        return 1u;
    }
}

// ============================================================
// sendException()
// ============================================================
void ModbusTcpSlave::sendException(uint8_t sn, const uint8_t* req,
                                    uint8_t fc, uint8_t exCode)
{
    // Exception response ADU:
    // MBAP (7 bytes): TID(2) + PID(2) + Length(2) + UnitID(1)
    // PDU  (2 bytes): FC|0x80 (1) + Exception code (1)
    // Total = 9 bytes

    uint8_t resp[9];

    // MBAP
    resp[0] = req[0];   // Transaction ID hi
    resp[1] = req[1];   // Transaction ID lo
    resp[2] = 0x00;     // Protocol ID hi
    resp[3] = 0x00;     // Protocol ID lo
    resp[4] = 0x00;     // Length hi — (UnitID + FC + ExCode) = 3
    resp[5] = 0x03;     // Length lo
    resp[6] = req[6];   // Unit ID (echo)

    // PDU
    resp[7] = static_cast<uint8_t>(fc | 0x80u); // Error FC
    resp[8] = exCode;                            // Exception code

    send(sn, resp, 9u);
}
