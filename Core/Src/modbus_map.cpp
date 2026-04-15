/**
 * ================================================================
 * @file    modbus_map.cpp
 * @brief   Modbus register map — conversion & calibration logic.
 *
 * @note    Estimated Flash: ~0.8 KB, RAM: 0 (uses RuntimeConfig).
 * ================================================================
 */
#include "modbus_map.hpp"
#include <cstring>

namespace ModbusMap {

uint8_t getEntryCount() {
    return Cfg().modbus_map_count;
}

const ModbusRegEntry* getEntry(uint8_t idx) {
    if (idx >= Cfg().modbus_map_count) return nullptr;
    return &Cfg().modbus_map[idx];
}

uint8_t getEntriesForPort(uint8_t port_idx, const ModbusRegEntry* out[], uint8_t max) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < Cfg().modbus_map_count && count < max; i++) {
        if (Cfg().modbus_map[i].port_idx == port_idx) {
            out[count++] = &Cfg().modbus_map[i];
        }
    }
    return count;
}

int16_t int16FromRegs(const uint16_t* regs) {
    return static_cast<int16_t>(regs[0]);
}

uint32_t uint32FromRegs(const uint16_t* regs) {
    return (static_cast<uint32_t>(regs[0]) << 16) | static_cast<uint32_t>(regs[1]);
}

float floatFromRegs(const uint16_t* regs) {
    // Big-endian word order (AB CD): reg[0]=high word, reg[1]=low word
    uint32_t raw = uint32FromRegs(regs);
    float result;
    std::memcpy(&result, &raw, sizeof(float));
    return result;
}

float convertByType(const uint16_t* regs, uint8_t data_type) {
    switch (data_type) {
        case 0: return static_cast<float>(int16FromRegs(regs));
        case 1: return static_cast<float>(uint32FromRegs(regs));
        case 2: return floatFromRegs(regs);
        default: return static_cast<float>(int16FromRegs(regs));
    }
}

float applyCalibration(float raw, const ModbusRegEntry& entry) {
    return (raw - entry.zero_offset) * entry.scale * entry.multiplier;
}

} // namespace ModbusMap

// Estimated Flash: ~0.8 KB  |  RAM: 0 bytes (references RuntimeConfig)
