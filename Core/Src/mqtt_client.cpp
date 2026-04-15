/**
 * ================================================================
 * @file    mqtt_client.cpp
 * @brief   MQTT client — W5500 (Paho/ioLibrary) and GSM backends.
 *
 * @note    W5500 uses ioLibrary_Driver MQTT stack (socket-based).
 *          GSM uses Air780E AT+CMQTT commands.
 *          Estimated Flash: ~3 KB, RAM: ~2.1 KB (tx/rx buffers).
 * ================================================================
 */
#include "mqtt_client.hpp"
#include "debug_uart.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>

extern "C" {
#include "socket.h"
#include "dns.h"
#include "wizchip_conf.h"
#include "MQTTClient.h"
}

// Undefine conflicting macros from ioLibrary
// socket.h: #define connect(...) CHOOSE_TESTCODE_MACRO(...)  -- conflicts with MqttClient::connect()
// wizchip_conf.h: #define W5500 5500  -- conflicts with MqttBackend::WIZNET
#ifdef connect
#  undef connect
#endif
#ifdef W5500
#  undef W5500
#endif

// ================================================================
// Static ioLibrary MQTT client + network + timer
// ================================================================
static Network       s_mqttNet;
static MQTTClient    s_mqttClient;
static Timer         s_mqttTimer;

// ================================================================
// Constructor
// ================================================================
MqttClient::MqttClient() {
    std::memset(m_txBuf, 0, sizeof(m_txBuf));
    std::memset(m_rxBuf, 0, sizeof(m_rxBuf));
}

// ================================================================
// Init
// ================================================================
void MqttClient::init(Air780E* gsm) {
    m_gsm = gsm;
    m_connected = false;
    DBG.info("MQTT: client init");
}

// ================================================================
// DNS resolve helper (reused from app.cpp pattern)
// ================================================================
static bool resolveHost(const char* host, uint8_t ip[4]) {
    // Check if numeric IP
    bool isNum = true;
    for (const char* p = host; *p; p++) {
        if (!std::isdigit((unsigned char)*p) && *p != '.') { isNum = false; break; }
    }

    if (isNum) {
        unsigned a=0,b=0,c=0,d=0;
        if (std::sscanf(host, "%u.%u.%u.%u", &a,&b,&c,&d) != 4) return false;
        ip[0]=(uint8_t)a; ip[1]=(uint8_t)b; ip[2]=(uint8_t)c; ip[3]=(uint8_t)d;
        return true;
    }

    // DNS resolve via W5500
    wiz_NetInfo ni{};
    wizchip_getnetinfo(&ni);
    static uint8_t dnsBuf[512];
    DNS_init(1, dnsBuf);
    DBG.info("MQTT DNS: resolve %s...", host);

    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 5000) {
        int8_t r = DNS_run(ni.dns, (uint8_t*)host, ip);
        if (r == 1) return true;
        if (r < 0)  return false;
        HAL_Delay(50);
        IWDG->KR = 0xAAAA;
    }
    return false;
}

// ================================================================
// W5500 MQTT connect (plain TCP, no TLS for now)
// ================================================================
bool MqttClient::w5500Connect() {
    const RuntimeConfig& c = Cfg();

    if (c.mqtt_host[0] == 0) {
        DBG.error("MQTT: no host configured");
        return false;
    }

    uint8_t brokerIp[4]{};
    if (!resolveHost(c.mqtt_host, brokerIp)) {
        DBG.error("MQTT: DNS resolve failed for %s", c.mqtt_host);
        return false;
    }

    DBG.info("MQTT: connecting to %s (%u.%u.%u.%u:%u)",
             c.mqtt_host, brokerIp[0], brokerIp[1], brokerIp[2], brokerIp[3],
             (unsigned)c.mqtt_port);

    // Initialize W5500 socket-based network
    NewNetwork(&s_mqttNet, MQTT_SOCKET);

    // TCP connect
    int rc = ConnectNetwork(&s_mqttNet, brokerIp, c.mqtt_port);
    if (rc != SOCK_OK) {
        DBG.error("MQTT: TCP connect failed rc=%d", rc);
        return false;
    }

    // Initialize MQTT client
    MQTTClientInit(&s_mqttClient, &s_mqttNet, 15000,
                   m_txBuf, MQTT_BUF_SIZE,
                   m_rxBuf, MQTT_BUF_SIZE);

    // Build connect options
    MQTTPacket_connectData opts = MQTTPacket_connectData_initializer;
    opts.MQTTVersion = 4;  // MQTT 3.1.1

    // Client ID
    static char clientId[32];
    std::snprintf(clientId, sizeof(clientId), "MonSet_%08lX", (unsigned long)HAL_GetTick());
    opts.clientID.cstring = clientId;

    opts.keepAliveInterval = 60;
    opts.cleansession = 1;

    if (c.mqtt_user[0]) {
        opts.username.cstring = (char*)c.mqtt_user;
    }
    if (c.mqtt_pass[0]) {
        opts.password.cstring = (char*)c.mqtt_pass;
    }

    rc = MQTTConnect(&s_mqttClient, &opts);
    if (rc != SUCCESSS) {
        DBG.error("MQTT: CONNECT failed rc=%d", rc);
        close(MQTT_SOCKET);
        return false;
    }

    DBG.info("MQTT: connected to %s", c.mqtt_host);
    m_connected = true;
    return true;
}

// ================================================================
// W5500 MQTT publish
// ================================================================
bool MqttClient::w5500Publish(const char* topic, const char* payload, uint16_t len) {
    if (!m_connected) return false;

    const RuntimeConfig& c = Cfg();
    const char* t = (topic && topic[0]) ? topic : c.mqtt_topic;

    MQTTMessage msg;
    msg.qos = (enum QoS)c.mqtt_qos;
    msg.retained = 0;
    msg.dup = 0;
    msg.payload = (void*)payload;
    msg.payloadlen = len;

    int rc = MQTTPublish(&s_mqttClient, t, &msg);
    if (rc != SUCCESSS) {
        DBG.error("MQTT: publish failed rc=%d", rc);
        m_connected = false;
        return false;
    }

    DBG.info("MQTT: published %u bytes to %s", (unsigned)len, t);
    return true;
}

// ================================================================
// W5500 MQTT disconnect
// ================================================================
void MqttClient::w5500Disconnect() {
    if (m_connected) {
        MQTTDisconnect(&s_mqttClient);
        close(MQTT_SOCKET);
        m_connected = false;
        DBG.info("MQTT: W5500 disconnected");
    }
}

// ================================================================
// GSM MQTT connect
// ================================================================
bool MqttClient::gsmConnect() {
    if (!m_gsm) {
        DBG.error("MQTT: GSM modem not available");
        return false;
    }

    const RuntimeConfig& c = Cfg();
    if (c.mqtt_host[0] == 0) {
        DBG.error("MQTT: no host configured");
        return false;
    }

    GsmStatus st = m_gsm->mqttConnect(c.mqtt_host, c.mqtt_port);
    if (st != GsmStatus::Ok) {
        DBG.error("MQTT: GSM connect failed");
        return false;
    }

    m_connected = true;
    DBG.info("MQTT: GSM connected to %s:%u", c.mqtt_host, (unsigned)c.mqtt_port);
    return true;
}

// ================================================================
// GSM MQTT publish
// ================================================================
bool MqttClient::gsmPublish(const char* topic, const char* payload, uint16_t len) {
    if (!m_gsm || !m_connected) return false;

    const RuntimeConfig& c = Cfg();
    const char* t = (topic && topic[0]) ? topic : c.mqtt_topic;

    GsmStatus st = m_gsm->mqttPublish(t, payload, c.mqtt_qos);
    if (st != GsmStatus::Ok) {
        DBG.error("MQTT: GSM publish failed");
        m_connected = false;
        return false;
    }

    DBG.info("MQTT: GSM published %u bytes to %s", (unsigned)len, t);
    return true;
}

// ================================================================
// GSM MQTT disconnect
// ================================================================
void MqttClient::gsmDisconnect() {
    if (m_gsm && m_connected) {
        m_gsm->mqttDisconnect();
        m_connected = false;
        DBG.info("MQTT: GSM disconnected");
    }
}

// ================================================================
// Public API
// ================================================================
bool MqttClient::mqttConnect(MqttBackend backend) {
    disconnect(); // clean up any previous connection

    m_backend = backend;

    switch (backend) {
        case MqttBackend::WIZNET: return w5500Connect();
        case MqttBackend::GSM:   return gsmConnect();
        default: return false;
    }
}

bool MqttClient::publish(const char* topic, const char* payload, uint16_t len) {
    switch (m_backend) {
        case MqttBackend::WIZNET: return w5500Publish(topic, payload, len);
        case MqttBackend::GSM:   return gsmPublish(topic, payload, len);
        default: return false;
    }
}

void MqttClient::disconnect() {
    switch (m_backend) {
        case MqttBackend::WIZNET: w5500Disconnect(); break;
        case MqttBackend::GSM:   gsmDisconnect();   break;
        default: break;
    }
}

// Estimated Flash: ~3 KB  |  RAM: ~2.1 KB (tx/rx buffers + static MQTT state)
