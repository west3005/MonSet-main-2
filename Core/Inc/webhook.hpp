/**
 * ================================================================
 * @file    webhook.hpp
 * @brief   HTTP webhook sender with payload template substitution.
 *
 * @note    Supports POST to arbitrary URL via W5500 or GSM.
 *          Templates: {{value}}, {{timestamp}}, {{sensor_name}}.
 *          Trigger modes: on-event or scheduled interval.
 * ================================================================
 */
#pragma once

#include "runtime_config.hpp"
#include "air780e.hpp"
#include <cstdint>

class Webhook {
public:
    Webhook();

    /// @brief Initialize with GSM modem reference (for GSM HTTP POST)
    void init(Air780E* gsm);

    /// @brief Send webhook with raw JSON payload
    /// @param json  JSON payload string
    /// @return true on success
    bool send(const char* json);

    /// @brief Send webhook with template substitution
    /// @param value       Sensor value
    /// @param timestamp   ISO8601 timestamp string
    /// @param sensorName  Sensor name string
    /// @return true on success
    bool sendTemplated(float value, const char* timestamp, const char* sensorName);

    /// @brief Check if scheduled trigger is due
    /// @return true if enough time has elapsed since last send
    bool isScheduledDue() const;

    /// @brief Mark that a scheduled send was completed
    void markScheduledSent();

    /// @brief Check if webhook is configured (has URL)
    bool isConfigured() const;

private:
    Air780E* m_gsm = nullptr;
    uint32_t m_lastScheduledTick = 0;

    /// @brief Substitute template variables in payload
    /// @param tmpl     Template string with {{var}} placeholders
    /// @param out      Output buffer
    /// @param outSz    Output buffer size
    /// @param value    Sensor value
    /// @param ts       Timestamp string
    /// @param name     Sensor name
    /// @return length of output, or -1 on error
    int substituteTemplate(const char* tmpl, char* out, size_t outSz,
                           float value, const char* ts, const char* name);

    /// @brief HTTP POST via W5500 plain socket
    bool postViaW5500(const char* url, const char* payload, uint16_t len);

    /// @brief HTTP POST via GSM modem
    bool postViaGsm(const char* url, const char* payload, uint16_t len);
};
