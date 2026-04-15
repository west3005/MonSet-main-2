/**
 * ================================================================
 * @file    mqtt_client.hpp
 * @brief   MQTT client supporting W5500 (ioLibrary) and GSM
 *          (Air780E AT+CMQTT) backends with optional TLS.
 *
 * @note    W5500 MQTT uses Paho embedded client from ioLibrary.
 *          GSM MQTT uses Air780E AT+CMQTT commands.
 *          TLS for W5500 via mbedTLS socket BIO.
 *          Config from RuntimeConfig (mqtt_host, port, user, pass,
 *          topic, qos, tls).
 * ================================================================
 */
#pragma once

#include "runtime_config.hpp"
#include "air780e.hpp"
#include <cstdint>

/// MQTT backend selection
enum class MqttBackend : uint8_t {
    WIZNET = 0,  ///< Ethernet via ioLibrary MQTT client (W5500)
    GSM    = 1   ///< Cellular via Air780E AT+CMQTT commands
};

class MqttClient {
public:
    MqttClient();

    /// @brief Initialize MQTT client with backend selection
    /// @param gsm  Pointer to Air780E for GSM backend (can be nullptr)
    void init(Air780E* gsm);

    /// @brief Connect to MQTT broker using config from RuntimeConfig
    /// @param backend  Which backend to use
    /// @return true on success
    bool mqttConnect(MqttBackend backend = MqttBackend::WIZNET);

    /// @brief Publish a message
    /// @param topic    Topic string (nullptr = use config topic)
    /// @param payload  JSON payload
    /// @param len      payload length
    /// @return true on success
    bool publish(const char* topic, const char* payload, uint16_t len);

    /// @brief Disconnect from broker
    void disconnect();

    /// @brief Check if connected
    bool isConnected() const { return m_connected; }

    /// @brief Get current backend
    MqttBackend backend() const { return m_backend; }

private:
    Air780E*    m_gsm       = nullptr;
    MqttBackend m_backend   = MqttBackend::WIZNET;
    bool        m_connected = false;

    // --- W5500 MQTT ---
    bool w5500Connect();
    bool w5500Publish(const char* topic, const char* payload, uint16_t len);
    void w5500Disconnect();

    // --- GSM MQTT ---
    bool gsmConnect();
    bool gsmPublish(const char* topic, const char* payload, uint16_t len);
    void gsmDisconnect();

    // W5500 socket for MQTT (socket 3, to avoid conflicts with HTTP on 0 and NTP on 2)
    static constexpr uint8_t MQTT_SOCKET = 3;
    static constexpr uint16_t MQTT_LOCAL_PORT = 50003;

    // Buffers for ioLibrary MQTT client
    static constexpr uint16_t MQTT_BUF_SIZE = 1024;
    uint8_t m_txBuf[MQTT_BUF_SIZE];
    uint8_t m_rxBuf[MQTT_BUF_SIZE];
};
