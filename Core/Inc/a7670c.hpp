/**
 * @file a7670c.hpp
 * @brief Драйвер A7670C LTE Cat-1 / GSM.
 *
 * Наследует Air780E — совместим с MqttClient, Webhook, Air780eTls.
 * Переопределяет методы под AT-команды A7670C:
 *   powerOn/powerOff/hardReset — другой PWRKEY (1500 мс) и URC "ME PDN ACT"
 *   init        — CREG+CEREG, autobaud, фиксация скорости
 *   httpPost    — AT+CSOC/CSOCON/CSOSEND/CSORCVDATA (не HTTPINIT)
 *   disconnect  — AT+CSOCL + CGACT=0
 *
 * AT-команды: A76XX Series AT Command Manual
 */
#pragma once
#include "air780e.hpp"

class A7670C : public Air780E {
public:
    A7670C(UART_HandleTypeDef* uart,
           GPIO_TypeDef*        pwrPort,
           uint16_t             pwrPin);

    // --- Переопределяем под A7670C ---
    void      powerOn();
    void      powerOff();
    void      hardReset();

    GsmStatus init();
    void      disconnect();

    uint16_t  httpPost(const char* url, const char* json, uint16_t len);

    uint8_t   getSignalQuality();

    // --- Публичные методы для A7670CTls ---
    void      sendRaw_pub(const char* data, uint16_t len) { sendRaw(data, len); }
    uint16_t  waitFor_pub(char* buf, uint16_t bsize,
                          const char* expected, uint32_t timeout)
              { return waitFor(buf, bsize, expected, timeout); }

    static constexpr uint8_t HTTP_SOCK_IDX = 0;
    static constexpr uint8_t TLS_SOCK_IDX  = 1;

    // --- MQTT (переопределяем под A7670C AT+CMQNEW/CMQCON/CMQPUB) ---
    GsmStatus mqttConnect(const char* broker, uint16_t port,
                          const char* clientId,
                          const char* user, const char* pass);
    GsmStatus mqttPublish(const char* topic, const char* payload, uint16_t len);
    void      mqttDisconnect();

private:
    bool      waitRdyA7670(uint32_t timeoutMs);
    GsmStatus activatePdnA7670();
};
