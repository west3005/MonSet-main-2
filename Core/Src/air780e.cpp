/**
 * ================================================================
 * @file    air780e.cpp
 * @brief   Драйвер GSM A7670E CAT1.
 *
 * Ключевые отличия от Air780E:
 *   - PWRKEY: достаточно 50 мс (вместо 1000 мс у Air780E)
 *   - STATUS поднимается через ~11.2 с (Ton(status) по даташиту A7670)
 *   - Команда выключения: AT+CPOF (вместо AT+CPWROFF)
 *   - PDN активируется тем же набором AT+CGDCONT / AT+CGACT
 *   - HTTP(S): AT+HTTPINIT / HTTPPARA / HTTPDATA / HTTPACTION (идентично)
 *   - MQTT   : AT+CMQTTSTART / CMQTTACCQ / CMQTTCONNECT / CMQTTPUB (идентично)
 *   - AT+CTZU=1 поддерживается (NITZ авто-время)
 * ================================================================
 */
#include "air780e.hpp"
#include "debug_uart.hpp"
#include "board_pins.hpp"
#include "uart_ringbuf.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

// ============================================================================
// Конструктор
// ============================================================================
Air780E::Air780E(UART_HandleTypeDef* uart,
                 GPIO_TypeDef*        pwrPort,
                 uint16_t             pwrPin)
    : m_uart(uart), m_pwrPort(pwrPort), m_pwrPin(pwrPin)
{}

// ============================================================================
// Низкоуровневый I/O
// ============================================================================
void Air780E::sendRaw(const char* data, uint16_t len)
{
    if (!m_uart || !data || len == 0) return;
    HAL_UART_Transmit(m_uart,
                      reinterpret_cast<const uint8_t*>(data),
                      len, 1000);
}

uint16_t Air780E::readResponse(char* buf, uint16_t bsize, uint32_t timeout)
{
    if (!buf || bsize < 2) return 0;

    uint16_t idx      = 0;
    uint32_t start    = HAL_GetTick();
    uint32_t lastByte = start;
    std::memset(buf, 0, bsize);

    while (idx < (uint16_t)(bsize - 1))
    {
        uint8_t ch;
        if (g_air780_rxbuf.pop(ch)) {
            buf[idx++] = static_cast<char>(ch);
            lastByte = HAL_GetTick();
            if (idx >= 4) {
                if (std::strstr(buf, "OK\r\n")    ||
                    std::strstr(buf, "ERROR\r\n")  ||
                    std::strstr(buf, "+CME ERROR") ||
                    std::strstr(buf, "+CMS ERROR"))
                    break;
            }
        } else {
            uint32_t now = HAL_GetTick();
            if (idx > 0 && (now - lastByte) >= 50) break;
            if ((now - start) >= timeout) break;
            HAL_Delay(1);
        }
        IWDG->KR = 0xAAAA;
    }
    buf[idx] = '\0';
    return idx;
}

uint16_t Air780E::waitFor(char* buf, uint16_t bsize,
                           const char* expected, uint32_t timeout)
{
    if (!buf || !expected || bsize < 2) return 0;

    uint16_t idx      = 0;
    uint32_t start    = HAL_GetTick();
    uint32_t lastByte = start;
    std::memset(buf, 0, bsize);

    while (idx < (uint16_t)(bsize - 1))
    {
        uint8_t ch;
        if (g_air780_rxbuf.pop(ch)) {
            buf[idx++] = static_cast<char>(ch);
            lastByte = HAL_GetTick();
            if (std::strstr(buf, expected)     ||
                std::strstr(buf, "+CME ERROR") ||
                std::strstr(buf, "ERROR\r\n"))
                break;
        } else {
            uint32_t now = HAL_GetTick();
            if (idx > 0 && (now - lastByte) >= 50) break;
            if ((now - start) >= timeout) break;
            HAL_Delay(1);
        }
        IWDG->KR = 0xAAAA;
    }
    buf[idx] = '\0';
    return idx;
}

GsmStatus Air780E::sendCommand(const char* cmd,
                                char*       resp,
                                uint16_t    rsize,
                                uint32_t    timeout)
{
    if (!resp || rsize == 0) return GsmStatus::Timeout;

    char at[256];
    std::snprintf(at, sizeof(at), "AT%s\r\n", cmd ? cmd : "");
    DBG.data("[A7670>] %s", at);

    g_air780_rxbuf.clear();
    sendRaw(at, (uint16_t)std::strlen(at));
    readResponse(resp, rsize, timeout);

    DBG.data("[A7670<] %s", resp);

    if (std::strstr(resp, "OK"))    return GsmStatus::Ok;
    if (std::strstr(resp, "ERROR")) return GsmStatus::HttpErr;
    return GsmStatus::Timeout;
}

// ============================================================================
// Управление питанием
// ============================================================================

/**
 * @brief Ожидание готовности модуля после включения.
 *        A7670E выдаёт "RDY" / "PB DONE" / "SMS Ready" на UART после загрузки.
 *        Ton(status) = 11.2 с по даташиту — задаём таймаут Config::SIM7020_BOOT_MS.
 */
bool Air780E::waitRdy(uint32_t timeoutMs)
{
    char buf[256]{};
    uint16_t idx = 0;
    uint32_t start = HAL_GetTick();

    g_air780_rxbuf.clear();

    while ((HAL_GetTick() - start) < timeoutMs) {
        uint8_t ch;
        if (g_air780_rxbuf.pop(ch)) {
            if (idx < sizeof(buf) - 1) buf[idx++] = static_cast<char>(ch);
            if (std::strstr(buf, "PB DONE") ||
                std::strstr(buf, "SMS Ready") ||
                std::strstr(buf, "SMS DONE")  ||
                std::strstr(buf, "RDY")) {
                DBG.info("A7670E: готов за %lu мс",
                         (unsigned long)(HAL_GetTick() - start));
                HAL_Delay(200);
                return true;
            }
            if (idx >= sizeof(buf) - 10) {
                std::memmove(buf, buf + 128, idx - 128);
                idx -= 128;
            }
        } else {
            HAL_Delay(1);
        }
        IWDG->KR = 0xAAAA;
    }
    DBG.warn("A7670E: RDY не получен за %lu мс", (unsigned long)timeoutMs);
    return false;
}

/**
 * @brief Выключение A7670E.
 *        Предпочтительный способ — AT+CPOF.
 *        Если STATUS не упал за 5 с — аппаратный PWRKEY (≥2.5 с LOW).
 *        После выключения снимаем PWR_EN.
 */
void Air780E::powerOff()
{
    DBG.info("A7670E: выключение...");

    if (HAL_GPIO_ReadPin(PIN_CELL_STATUS_PORT, PIN_CELL_STATUS_PIN) == GPIO_PIN_RESET) {
        DBG.info("A7670E: уже выключен (STATUS=LOW), снимаем PWR_EN");
        HAL_GPIO_WritePin(m_pwrPort, m_pwrPin, GPIO_PIN_RESET);
        return;
    }

    // Программное выключение (A7670E: AT+CPOF, совместимо с A76XX AT Manual)
    char r[64];
    sendCommand("+CPOF", r, sizeof(r), 3000);

    // Ждём пока STATUS упадёт в LOW (до 5 с)
    uint32_t t = HAL_GetTick();
    while (HAL_GPIO_ReadPin(PIN_CELL_STATUS_PORT, PIN_CELL_STATUS_PIN) == GPIO_PIN_SET) {
        if (HAL_GetTick() - t > 5000) {
            // Принудительное выключение PWRKEY LOW ≥ 2.5 с
            DBG.warn("A7670E: принудительное выключение PWRKEY (2500 мс)");
            HAL_GPIO_WritePin(PIN_CELL_PWRKEY_PORT, PIN_CELL_PWRKEY_PIN, GPIO_PIN_RESET);
            HAL_Delay(2600);
            HAL_GPIO_WritePin(PIN_CELL_PWRKEY_PORT, PIN_CELL_PWRKEY_PIN, GPIO_PIN_SET);
            HAL_Delay(2000);
            break;
        }
        HAL_Delay(100);
        IWDG->KR = 0xAAAA;
    }

    HAL_Delay(500);
    HAL_GPIO_WritePin(m_pwrPort, m_pwrPin, GPIO_PIN_RESET);  // снять PWR_EN
    HAL_Delay(1000);
    g_air780_rxbuf.clear();
}

/**
 * @brief Включение A7670E.
 *        1. Подаём PWR_EN (питание DC-DC платы).
 *        2. Задержка 1000 мс для стабилизации VBAT.
 *        3. Если STATUS уже HIGH — модуль уже включён.
 *        4. Иначе импульс PWRKEY LOW ≥ 50 мс (typ по даташиту A7670).
 *        5. Ждём "PB DONE" / "RDY" (Ton = до 11.2 с).
 */
void Air780E::powerOn()
{
    DBG.info("A7670E: включение питания...");

    // Подаём PWR_EN → включаем DC-DC конвертер на плате
    HAL_GPIO_WritePin(m_pwrPort, m_pwrPin, GPIO_PIN_SET);
    HAL_Delay(1000);  // ждём стабилизацию VBAT (пик тока до 2A при регистрации)

    if (HAL_GPIO_ReadPin(PIN_CELL_STATUS_PORT, PIN_CELL_STATUS_PIN) == GPIO_PIN_SET) {
        DBG.info("A7670E: уже включён (STATUS=HIGH)");
        g_air780_rxbuf.clear();
        HAL_Delay(500);
        return;
    }

    // Импульс PWRKEY: LOW ≥ 50 мс → используем 100 мс для надёжности
    DBG.info("A7670E: подача PWRKEY (100 мс)...");
    HAL_GPIO_WritePin(PIN_CELL_PWRKEY_PORT, PIN_CELL_PWRKEY_PIN, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(PIN_CELL_PWRKEY_PORT, PIN_CELL_PWRKEY_PIN, GPIO_PIN_SET);

    // Ждём "PB DONE" (Ton ≈ 11.2 с)
    DBG.info("A7670E: ожидание готовности (PB DONE / RDY)...");
    if (!waitRdy(Config::SIM7020_BOOT_MS)) {
        DBG.warn("A7670E: RDY не получен — продолжаем");
    }
}

/**
 * @brief Аппаратный сброс A7670E.
 *        Пин RESET на плате CAT1-7670X-V1.02 не выведен.
 *        Реализуем как powerOff + powerOn.
 */
void Air780E::hardReset()
{
    DBG.warn("A7670E: аппаратный сброс (power cycle)");
    // Попытка через RESET пин (если подключён к PD7)
    HAL_GPIO_WritePin(PIN_CELL_RESET_PORT, PIN_CELL_RESET_PIN, GPIO_PIN_RESET);
    HAL_Delay(300);
    HAL_GPIO_WritePin(PIN_CELL_RESET_PORT, PIN_CELL_RESET_PIN, GPIO_PIN_SET);
    waitRdy(Config::SIM7020_BOOT_MS);
}

// ============================================================================
// Инициализация PDN (Packet Data Network)
// ============================================================================
GsmStatus Air780E::activatePdn()
{
    const RuntimeConfig& c = Cfg();
    char r[256], cmd[128];

    // Задать APN контекст 1
    std::snprintf(cmd, sizeof(cmd),
                  "+CGDCONT=1,\"IP\",\"%s\"", c.gsm_apn);
    sendCommand(cmd, r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS);

    // Активировать PDP контекст (A7670E поддерживает AT+CGACT)
    sendCommand("+CGACT=1,1", r, sizeof(r), Config::SIM7020_PDN_TIMEOUT_MS);

    // Проверить регистрацию
    sendCommand("+CEREG?", r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS);
    DBG.info("A7670E: CEREG=%s", r);

    DBG.info("A7670E: APN настроен");
    return GsmStatus::Ok;
}

// ============================================================================
// Инициализация модуля
// ============================================================================
GsmStatus Air780E::init()
{
    char r[256];

    g_air780_rxbuf.clear();
    __HAL_UART_FLUSH_DRREGISTER(m_uart);
    HAL_Delay(100);

    DBG.info("A7670E: проверка связи...");

    // Выключить эхо (3 попытки)
    for (uint8_t i = 0; i < 3; i++) {
        sendCommand("E0", r, sizeof(r), 1000);
        HAL_Delay(200);
        IWDG->KR = 0xAAAA;
    }

    // Проверка AT (10 попыток × 2 с = 20 с max)
    bool alive = false;
    for (uint8_t i = 0; i < 10 && !alive; i++) {
        if (sendCommand("", r, sizeof(r), 2000) == GsmStatus::Ok)
            alive = true;
        else
            HAL_Delay(500);
        IWDG->KR = 0xAAAA;
    }
    if (!alive) {
        DBG.error("A7670E: нет ответа на AT");
        return GsmStatus::Timeout;
    }
    DBG.info("A7670E: связь OK");

    sendCommand("E0", r, sizeof(r), 2000);  // ATE0 повторно

    // Идентификация
    {
        char info[128];
        sendCommand("I", info, sizeof(info), 2000);
        DBG.info("A7670E: %s", info);
        sendCommand("+GMR", info, sizeof(info), 2000);
        DBG.info("A7670E FW: %s", info);
    }

    // Включить авто-синхронизацию времени из сети (NITZ) — для валидации TLS сертификата
    sendCommand("+CTZU=1", r, sizeof(r), 2000);
    DBG.info("A7670E: CTZU (авто-время) включён");

    // Проверить SIM (5 попыток)
    {
        char sim[64];
        bool simReady = false;
        for (uint8_t i = 0; i < 5 && !simReady; i++) {
            sendCommand("+CPIN?", sim, sizeof(sim), 5000);
            if (std::strstr(sim, "READY")) {
                simReady = true;
            } else {
                HAL_Delay(1000);
                IWDG->KR = 0xAAAA;
            }
        }
        if (!simReady) {
            DBG.error("A7670E: SIM не готова — %s", sim);
            return GsmStatus::NoSim;
        }
    }
    DBG.info("A7670E: SIM OK");

    // Ожидание регистрации в сети (30 попыток × 2 с = 60 с max)
    {
        bool registered = false;
        for (uint8_t i = 0; i < 30 && !registered; i++) {
            sendCommand("+CREG?", r, sizeof(r), 2000);
            // Статусы: 1=зарегистрирован домашняя, 5=роуминг
            if (std::strstr(r, ",1") || std::strstr(r, ",5"))
                registered = true;
            else {
                HAL_Delay(2000);
                IWDG->KR = 0xAAAA;
            }
        }
        if (!registered) {
            DBG.error("A7670E: регистрация не выполнена");
            return GsmStatus::NoReg;
        }
    }
    DBG.info("A7670E: сеть OK");

    // Активация PDN
    if (activatePdn() != GsmStatus::Ok) {
        DBG.error("A7670E: PDN активация не удалась");
        return GsmStatus::PdnErr;
    }

    DBG.info("A7670E: init завершён");
    return GsmStatus::Ok;
}

// ============================================================================
// HTTP(S) POST
// Протокол идентичен Air780E (A7670E входит в то же семейство A76XX)
// ============================================================================
uint16_t Air780E::httpPost(const char* url, const char* json, uint16_t len)
{
    if (!url || !json || len == 0) return 0;

    char r[512], cmd[512];
    bool isHttps = (std::strncmp(url, "https://", 8) == 0);

    /* A7670E HTTP(S) POST sequence (SIMCOM A7670 AT Command Manual):
     *
     *  1. AT+HTTPTERM      — сбросить предыдущую сессию (ERROR если не была — норма)
     *  2. AT+HTTPINIT      — инициализировать HTTP стек
     *  3. AT+HTTPSSL=1     — включить TLS (только для HTTPS; без параметра индекса!)
     *  4. AT+HTTPPARA="NETPN",1  — привязать к PDP контексту #1
     *  5. AT+HTTPPARA="URL","..." — задать URL (HTTPS определяется по схеме)
     *  6. AT+HTTPPARA="CONTENT","application/json"
     *  7. AT+HTTPDATA=<len>,<timeout_ms> → ждём "DOWNLOAD"
     *  8. <тело> → ждём "OK"
     *  9. AT+HTTPACTION=1  → URC "+HTTPACTION: 1,<code>,<len>" (ждать до 60 с!)
     * 10. AT+HTTPTERM
     *
     * НЕ ПОДДЕРЖИВАЕТСЯ на A7670E:
     *   AT+HTTPSSL=1,0   (лишний параметр → ERROR)
     *   AT+HTTPPARA="CID",1  (нет такого параметра → ERROR)
     */

    // 1. Сброс предыдущей сессии (ERROR — норма если сессии нет)
    sendCommand("+HTTPTERM", r, sizeof(r), 2000);
    HAL_Delay(200);

    // 2. HTTPINIT
    if (sendCommand("+HTTPINIT", r, sizeof(r), 5000) != GsmStatus::Ok) {
        DBG.error("A7670E: HTTPINIT failed: %s", r);
        return 0;
    }

    // 3. SSL — только AT+HTTPSSL=1 (без второго параметра!)
    if (isHttps) {
        sendCommand("+HTTPSSL=1", r, sizeof(r), 2000);
        DBG.info("A7670E: HTTPSSL=1: %s", r);
    }

    // 4. PDP контекст — AT+HTTPPARA="NETPN",1  (не "CID"!)
    sendCommand("+HTTPPARA=\"NETPN\",1", r, sizeof(r), 2000);

    // 5. URL
    std::snprintf(cmd, sizeof(cmd), "+HTTPPARA=\"URL\",\"%s\"", url);
    if (sendCommand(cmd, r, sizeof(r), 3000) != GsmStatus::Ok) {
        DBG.error("A7670E: HTTPPARA URL failed: %s", r);
        sendCommand("+HTTPTERM", r, sizeof(r), 2000);
        return 0;
    }

    // 6. Content-Type
    sendCommand("+HTTPPARA=\"CONTENT\",\"application/json\"", r, sizeof(r), 2000);

    // 7. HTTPDATA — ждём DOWNLOAD prompt
    {
        char fullCmd[64];
        std::snprintf(fullCmd, sizeof(fullCmd),
                      "AT+HTTPDATA=%u,10000\r\n", (unsigned)len);
        g_air780_rxbuf.clear();
        sendRaw(fullCmd, (uint16_t)std::strlen(fullCmd));
    }
    waitFor(r, sizeof(r), "DOWNLOAD", 5000);
    if (!std::strstr(r, "DOWNLOAD")) {
        DBG.error("A7670E: нет DOWNLOAD prompt: [%s]", r);
        sendCommand("+HTTPTERM", r, sizeof(r), 2000);
        return 0;
    }

    // 8. Тело запроса → ждём OK
    g_air780_rxbuf.clear();
    sendRaw(json, len);
    readResponse(r, sizeof(r), 5000);
    DBG.info("A7670E: HTTPDATA body resp=[%s]", r);
    if (!std::strstr(r, "OK")) {
        DBG.warn("A7670E: HTTPDATA body нет OK, продолжаем...");
    }

    // 9. HTTPACTION=1 (POST) — ответ приходит как URC, ждём до 60 с для HTTPS
    g_air780_rxbuf.clear();
    sendRaw("AT+HTTPACTION=1\r\n", 17);
    /* A7670E сначала отвечает "OK" на саму команду, затем URC "+HTTPACTION: 1,200,nn"
     * Для HTTPS TLS handshake + передача может занять до 30-60 секунд. */
    uint32_t actionTimeout = isHttps ? 60000UL : 30000UL;
    waitFor(r, sizeof(r), "+HTTPACTION:", actionTimeout);

    /* Иногда +HTTPACTION: приходит отдельным URC после OK — дочитываем */
    if (!std::strstr(r, "+HTTPACTION:")) {
        char extra[256]{};
        readResponse(extra, sizeof(extra), 10000);
        if (std::strstr(extra, "+HTTPACTION:")) {
            std::strncat(r, extra, sizeof(r) - std::strlen(r) - 1);
        }
    }
    DBG.info("A7670E: HTTPACTION resp=[%s]", r);

    uint16_t httpCode = 0;
    const char* p = std::strstr(r, "+HTTPACTION:");
    if (p) {
        unsigned method = 0, code = 0, dlen = 0;
        /* Формат: "+HTTPACTION: 1,200,26" или "+HTTPACTION:1,200,26" */
        if (std::sscanf(p, "+HTTPACTION: %u,%u,%u", &method, &code, &dlen) < 2)
            std::sscanf(p, "+HTTPACTION:%u,%u,%u",  &method, &code, &dlen);
        httpCode = (uint16_t)code;
        DBG.info("A7670E: method=%u code=%u datalen=%u", method, code, dlen);
    } else {
        DBG.error("A7670E: +HTTPACTION не получен (timeout %lu ms)", actionTimeout);
    }

    // 10. Завершить сессию
    sendCommand("+HTTPTERM", r, sizeof(r), 2000);
    return httpCode;
}
void Air780E::disconnect()
{
    char r[64];
    sendCommand("+HTTPTERM", r, sizeof(r), 2000);
    mqttDisconnect();
    // Деактивировать PDP
    sendCommand("+CGACT=0,1", r, sizeof(r), 5000);
}

GsmStatus Air780E::mqttConnect(const char* broker, uint16_t port)
{
    if (!broker) return GsmStatus::MqttErr;

    char r[512], cmd[256];

    if (m_mqttStarted) {
        std::snprintf(cmd, sizeof(cmd), "+CMQTTDISC=%hhu,10", MQTT_IDX);
        sendCommand(cmd, r, sizeof(r), 5000);
        std::snprintf(cmd, sizeof(cmd), "+CMQTTREL=%hhu", MQTT_IDX);
        sendCommand(cmd, r, sizeof(r), 2000);
        sendCommand("+CMQTTSTOP", r, sizeof(r), 3000);
        m_mqttStarted = false;
        HAL_Delay(500);
    }

    if (sendCommand("+CMQTTSTART", r, sizeof(r), 5000) != GsmStatus::Ok) {
        DBG.error("A7670E MQTT: CMQTTSTART fail");
        return GsmStatus::MqttErr;
    }
    m_mqttStarted = true;

    char clientId[32];
    std::snprintf(clientId, sizeof(clientId),
                  "MonSet_%04X", (unsigned)(HAL_GetTick() & 0xFFFFu));
    std::snprintf(cmd, sizeof(cmd),
                  "+CMQTTACCQ=%hhu,\"%s\"", MQTT_IDX, clientId);
    if (sendCommand(cmd, r, sizeof(r), 3000) != GsmStatus::Ok) {
        DBG.error("A7670E MQTT: CMQTTACCQ fail");
        return GsmStatus::MqttErr;
    }

    std::snprintf(cmd, sizeof(cmd),
                  "+CMQTTCONNECT=%hhu,\"tcp://%s:%u\",60,1",
                  MQTT_IDX, broker, port);
    if (sendCommand(cmd, r, sizeof(r),
                    Config::SIM7020_MQTT_TIMEOUT_MS) != GsmStatus::Ok) {
        DBG.error("A7670E MQTT: CMQTTCONNECT fail — %s", r);
        return GsmStatus::MqttErr;
    }

    DBG.info("A7670E MQTT: подключён к %s:%u", broker, port);
    return GsmStatus::Ok;
}

GsmStatus Air780E::mqttPublish(const char* topic,
                                const char* payload,
                                uint8_t     qos)
{
    if (!topic || !payload) return GsmStatus::MqttErr;

    uint16_t topicLen   = (uint16_t)std::strlen(topic);
    uint16_t payloadLen = (uint16_t)std::strlen(payload);
    char r[256], cmd[128];

    {
        char fullCmd[64];
        std::snprintf(fullCmd, sizeof(fullCmd),
                      "AT+CMQTTTOPIC=%hhu,%u\r\n", MQTT_IDX, topicLen);
        __HAL_UART_FLUSH_DRREGISTER(m_uart);
        sendRaw(fullCmd, (uint16_t)std::strlen(fullCmd));
    }
    waitFor(r, sizeof(r), ">", 3000);
    if (!std::strstr(r, ">")) {
        DBG.error("A7670E MQTT: нет prompt для топика");
        return GsmStatus::MqttErr;
    }
    sendRaw(topic, topicLen);
    readResponse(r, sizeof(r), 2000);

    {
        char fullCmd[64];
        std::snprintf(fullCmd, sizeof(fullCmd),
                      "AT+CMQTTPAYLOAD=%hhu,%u\r\n", MQTT_IDX, payloadLen);
        __HAL_UART_FLUSH_DRREGISTER(m_uart);
        sendRaw(fullCmd, (uint16_t)std::strlen(fullCmd));
    }
    waitFor(r, sizeof(r), ">", 3000);
    if (!std::strstr(r, ">")) {
        DBG.error("A7670E MQTT: нет prompt для payload");
        return GsmStatus::MqttErr;
    }
    sendRaw(payload, payloadLen);
    readResponse(r, sizeof(r), 2000);

    std::snprintf(cmd, sizeof(cmd),
                  "+CMQTTPUB=%hhu,%hhu,60", MQTT_IDX, qos);
    if (sendCommand(cmd, r, sizeof(r),
                    Config::SIM7020_MQTT_TIMEOUT_MS) != GsmStatus::Ok) {
        DBG.error("A7670E MQTT: CMQTTPUB fail — %s", r);
        return GsmStatus::MqttErr;
    }

    DBG.info("A7670E MQTT: publish OK topic=%s", topic);
    return GsmStatus::Ok;
}

void Air780E::mqttDisconnect()
{
    if (!m_mqttStarted) return;
    char r[64], cmd[32];
    std::snprintf(cmd, sizeof(cmd), "+CMQTTDISC=%hhu,10", MQTT_IDX);
    sendCommand(cmd, r, sizeof(r), 5000);
    std::snprintf(cmd, sizeof(cmd), "+CMQTTREL=%hhu", MQTT_IDX);
    sendCommand(cmd, r, sizeof(r), 2000);
    sendCommand("+CMQTTSTOP", r, sizeof(r), 3000);
    m_mqttStarted = false;
}
