/**
 * @file    modbus_tcp_slave.hpp
 * @brief   Modbus TCP Slave — MonSet exposes measurements to SCADA/PLC via W5500.
 *
 * Supports:
 *   FC03 — Read Holding Registers (reg_map[] sources: sensor[0..3], battery_pct, battery_v)
 *   FC02 — Read Discrete Inputs (channel status bits)
 *
 * W5500 Socket 3, port 502 (configurable).
 * Non-blocking: call tick() from main loop each cycle.
 *
 * Data sources (source field in reg_map):
 *   0..3  = m_readings[0..3]  (float sensor values)
 *   4     = battery_pct       (uint16 integer %)
 *   5     = battery_v         (float, apply scale)
 *   6     = channel_status    (bit0=ETH,bit1=GSM,bit2=WIFI,bit3=IRIDIUM)
 */
#pragma once
#include "runtime_config.hpp"
#include <cstdint>

class ModbusTcpSlave {
public:
    ModbusTcpSlave() = default;

    /**
     * @brief Initialize slave (open socket, listen).
     * @param cfg   Slave config
     */
    void init(const ModbusTcpSlaveConfig& cfg);

    /**
     * @brief Non-blocking tick — process one pending client request.
     * @param cfg        Current slave config
     * @param readings   Sensor readings array (indexed by source 0..3)
     * @param battPct    Battery percent (integer)
     * @param battV      Battery voltage float
     * @param chanStatus Channel status bitmask (bit0=ETH active, etc.)
     */
    void tick(const ModbusTcpSlaveConfig& cfg,
              const float* readings, uint8_t battPct, float battV,
              uint8_t chanStatus);

    bool isRunning() const { return m_running; }

private:
    bool    m_running = false;
    uint8_t m_socket  = 3;

    // Request/response buffers
    static constexpr uint16_t BUF_SIZE = 260;
    uint8_t m_req[BUF_SIZE];
    uint8_t m_resp[BUF_SIZE];

    /**
     * @brief Handle a complete ADU request.
     * @param cfg        Slave config
     * @param readings   Sensor data
     * @param battPct    Battery %
     * @param battV      Battery voltage
     * @param chanStatus Channel bitmask
     */
    void handleRequest(const ModbusTcpSlaveConfig& cfg,
                       const float* readings, uint8_t battPct, float battV,
                       uint8_t chanStatus);

    /**
     * @brief Build FC03 response for requested register range.
     * @return Response length in bytes, or 0 on error
     */
    uint16_t buildFC03Response(const ModbusTcpSlaveConfig& cfg,
                                const float* readings, uint8_t battPct,
                                float battV, uint8_t chanStatus,
                                const uint8_t* req, uint8_t* resp,
                                uint16_t respMax);

    /**
     * @brief Build FC02 (Discrete Inputs) response for channel status.
     */
    uint16_t buildFC02Response(uint8_t chanStatus,
                                const uint8_t* req, uint8_t* resp,
                                uint16_t respMax);

    /**
     * @brief Get register value from data sources.
     * @param entry      Register map entry
     * @param readings   Sensor readings
     * @param battPct    Battery %
     * @param battV      Battery voltage
     * @param chanStatus Channel bitmask
     * @param outRegs    Output: 1 or 2 uint16 values (big-endian)
     * @return Number of registers written (1 for INT16/UINT16, 2 for FLOAT32)
     */
    static uint8_t getRegValue(const ModbusTcpSlaveRegEntry& entry,
                                const float* readings, uint8_t battPct,
                                float battV, uint8_t chanStatus,
                                uint16_t* outRegs);

    /**
     * @brief Send Modbus exception response
     * @param sn     Socket
     * @param req    Original request (to copy MBAP TID/PID)
     * @param fc     Original function code
     * @param exCode Exception code (e.g. 0x02=Illegal Data Address)
     */
    void sendException(uint8_t sn, const uint8_t* req, uint8_t fc, uint8_t exCode);
};
