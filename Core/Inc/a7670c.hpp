/**
 * @file a7670c.hpp
 * @brief Драйвер A7670C LTE Cat-1 / GSM (замена SIM7020C NB-IoT).
 *
 * AT-команды: A76XX Series AT Command Manual
 * TCP socket: AT+CSOC / AT+CSOCON / AT+CSOSEND / AT+CSORCVDATA
 * HTTP:       через TCP socket (порт 80) или TLS (порт 443 via a7670c_tls)
 * MQTT:       через нативный MQTT-стек (AT+CMQNEW/CMQCON/CMQPUB)
 */
#pragma once
#include "gsm_common.hpp"
#include "stm32f4xx_hal.h"
#include "runtime_config.hpp"
#include <cstdint>

class A7670C {
public:
    A7670C(UART_HandleTypeDef* uart,
           GPIO_TypeDef*        pwrPort,
           uint16_t             pwrPin);

    // Жизненный цикл
    void      powerOn();
    void      powerOff();
    void      hardReset();

    // Инициализация (вызывать после powerOn)
    GsmStatus init();
    void      disconnect();

    // HTTP POST через TCP socket (порт 80, без TLS)
    uint16_t  httpPost(const char* url, const char* json, uint16_t len);

    // MQTT (нативный стек модема)
    GsmStatus mqttConnect(const char* broker, uint16_t port,
                          const char* clientId, const char* user, const char* pass);
    GsmStatus mqttPublish(const char* topic, const char* payload, uint16_t len);
    void      mqttDisconnect();

    // Утилиты
    uint8_t   getSignalQuality();

    // --- Публичные методы для TLS-слоя (a7670c_tls) ---
    void      sendRaw_pub(const char* data, uint16_t len) { sendRaw(data, len); }
    uint16_t  waitFor_pub(char* buf, uint16_t bsize,
                          const char* expected, uint32_t timeout)
              { return waitFor(buf, bsize, expected, timeout); }

    static constexpr uint8_t HTTP_SOCK_IDX = 0;
    static constexpr uint8_t TLS_SOCK_IDX  = 1;

private:
    UART_HandleTypeDef* m_uart;
    GPIO_TypeDef*       m_pwrPort;
    uint16_t            m_pwrPin;

    void      sendRaw(const char* data, uint16_t len);
    uint16_t  readResponse(char* buf, uint16_t bsize, uint32_t timeout);
    uint16_t  waitFor(char* buf, uint16_t bsize,
                      const char* expected, uint32_t timeout);
    GsmStatus sendCommand(const char* cmd, char* resp,
                          uint16_t rsize, uint32_t timeout);

    bool      waitRdy(uint32_t timeoutMs);
    GsmStatus activatePdn();
};
