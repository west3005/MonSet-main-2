/**
 * @file    modbus_tcp_master.hpp
 * @brief   Modbus TCP Master — polls slave devices over W5500 TCP.
 *
 * Modbus TCP ADU = MBAP Header (6 bytes) + PDU
 * MBAP: TransactionID(2) + ProtocolID(2=0x0000) + Length(2) + UnitID(1)
 * PDU:  FunctionCode(1) + Data(...)
 *
 * W5500 socket 2 (configurable per device) — do not conflict with:
 *   Socket 0: HTTP server
 *   Socket 1: MQTT/HTTPS client
 *   Socket 3: Modbus TCP Slave
 */
#pragma once
#include "runtime_config.hpp"
#include <cstdint>

class ModbusTcpMaster {
public:
    ModbusTcpMaster() = default;

    /**
     * @brief Poll all enabled TCP devices in config.
     * @param cfg        Full runtime config
     * @param readings   Output array [MAX_TCP_DEVICES] — indexed by channel_idx
     * @param readingsMax Size of readings array
     */
    void pollAll(const ModbusTcpMasterConfig& cfg, float* readings, uint8_t readingsMax);

    /**
     * @brief Poll single device.
     * @param dev    Device config
     * @param result Output float value
     * @return true on success
     */
    bool readDevice(const ModbusTcpDeviceCfg& dev, float& result);

private:
    uint16_t m_transactionId = 0; ///< Auto-incrementing MBAP transaction ID

    /**
     * @brief Parse raw register bytes to float based on data_type
     * @param regs     Raw bytes (big-endian, 2 bytes per register)
     * @param dtype    0=INT16,1=UINT16,2=INT32_BE,3=UINT32_BE,4=FLOAT32_BE
     * @return Parsed float value
     */
    static float parseRegisters(const uint8_t* regs, uint8_t dtype);

    /**
     * @brief Build Modbus TCP request ADU into buf
     * @param buf      Output buffer (>= 12 bytes)
     * @param tid      Transaction ID
     * @param unitId   Modbus Unit ID
     * @param fc       Function code (3 or 4)
     * @param startReg Starting register address
     * @param regCount Number of registers
     * @return Length of ADU in bytes
     */
    static uint8_t buildRequest(uint8_t* buf, uint16_t tid, uint8_t unitId,
                                uint8_t fc, uint16_t startReg, uint8_t regCount);

    /**
     * @brief Wait for data on socket with timeout
     * @param sn        W5500 socket number
     * @param timeoutMs Timeout in milliseconds
     * @return bytes available, or 0 on timeout
     */
    static int32_t waitRecv(uint8_t sn, uint32_t timeoutMs);

    /**
     * @brief Parse an IPv4 string "a.b.c.d" into a 4-byte array
     * @param s   Null-terminated IP string
     * @param ip  Output 4-byte array
     * @return true on success, false if string is malformed
     */
    static bool parseIpStr(const char* s, uint8_t ip[4]);
};
