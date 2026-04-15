/**
 * ================================================================
 * @file    modbus_map.hpp
 * @brief   Modbus register map manager — parses config, provides
 *          typed data conversion with scale/offset.
 *
 * @note    Operates on the modbus_map[] array inside RuntimeConfig.
 *          No dynamic allocation.
 * ================================================================
 */
#pragma once

#include "runtime_config.hpp"
#include <cstdint>

namespace ModbusMap {

/// @brief Number of configured modbus map entries
uint8_t getEntryCount();

/// @brief Get entry by index (nullptr if out of range)
const ModbusRegEntry* getEntry(uint8_t idx);

/// @brief Get all entries for a given UART port index
/// @param port_idx  0=USART3, 1=UART4, 2=UART5
/// @param out       output array
/// @param max       max entries to fill
/// @return count of matching entries
uint8_t getEntriesForPort(uint8_t port_idx, const ModbusRegEntry* out[], uint8_t max);

/// @brief Convert two 16-bit registers to int16_t (uses reg[0])
int16_t int16FromRegs(const uint16_t* regs);

/// @brief Convert two 16-bit registers to uint32_t (reg[0] << 16 | reg[1])
uint32_t uint32FromRegs(const uint16_t* regs);

/// @brief Convert two 16-bit registers to IEEE 754 float (big-endian word order)
float floatFromRegs(const uint16_t* regs);

/// @brief Convert raw registers using data_type from ModbusRegEntry
float convertByType(const uint16_t* regs, uint8_t data_type);

/// @brief Apply scale, zero_offset, and multiplier to a raw value
///        result = (raw - zero_offset) * scale * multiplier
float applyCalibration(float raw, const ModbusRegEntry& entry);

} // namespace ModbusMap
