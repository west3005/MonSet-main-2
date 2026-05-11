/**
 * ================================================================
 * @file    a7670c.cpp
 * @brief   Реализация драйвера A7670C LTE Cat-1 / GSM.
 *
 * Протоколы:
 *   HTTP POST — через нативный TCP socket (AT+CSOC/CSOCON/CSOSEND/CSORCVDATA)
 *   MQTT      — через нативный MQTT-стек (AT+CMQNEW/CMQCON/CMQPUB)
 *   HTTPS     — через TLS поверх TCP (см. a7670c_tls.cpp)
 *
 * Отличия от SIM7020C:
 *   - Регистрация: AT+CREG (GSM) + AT+CEREG (LTE), fallback GSM/GPRS
 *   - TCP send:    AT+CSOSEND (не CSODSEND)
 *   - TCP recv:    AT+CSORCVDATA (не CSORCV)
 *   - Загрузка:    ждём "SMS DONE" + "+CGEV: ME PDN ACT"
 *   - PWRKEY:      pulse LOW 1500 мс (не 1200)
 *
 * AT-команды: A76XX Series AT Command Manual
 * ================================================================
 */
#include "a7670c.hpp"
#include "debug_uart.hpp"
#include "board_pins.hpp"
#include "config.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

// ============================================================================
// Вспомогательный парсинг URL
// ============================================================================
namespace {

struct UrlParts {
    char     host[64]{};
    char     path[128]{};
    uint16_t port = 80;
};

static bool parseHttpUrl(const char* url, UrlParts& out)
{
    if (!url) return false;
    out = UrlParts{};
    out.port = 80;

    const char* prefix = "http://";
    if (std::strncmp(url, prefix, std::strlen(prefix)) != 0) return false;

    const char* p = url + std::strlen(prefix);

    const char* hostBeg = p;
    while (*p && *p != '/' && *p != ':') p++;
    size_t hostLen = (size_t)(p - hostBeg);
    if (hostLen == 0 || hostLen >= sizeof(out.host)) return false;
    std::memcpy(out.host, hostBeg, hostLen);
    out.host[hostLen] = '\0';

    if (*p == ':') {
        p++;
        uint32_t port = 0;
        while (*p && std::isdigit((unsigned char)*p))
            port = port * 10u + (uint32_t)(*p++ - '0');
        if (port == 0 || port > 65535) return false;
        out.port = (uint16_t)port;
    }

    if (*p == '\0') { std::strcpy(out.path, "/"); return true; }
    if (*p != '/') return false;
    size_t pathLen = std::strlen(p);
    if (pathLen == 0 || pathLen >= sizeof(out.path)) return false;
    std::memcpy(out.path, p, pathLen + 1);
    return true;
}

} // namespace

// ============================================================================
// Конструктор
// ============================================================================
A7670C::A7670C(UART_HandleTypeDef* uart,
               GPIO_TypeDef*        pwrPort,
               uint16_t             pwrPin)
    : m_uart(uart), m_pwrPort(pwrPort), m_pwrPin(pwrPin)
{}

// ============================================================================
// Низкоуровневый I/O
// ============================================================================
void A7670C::sendRaw(const char* data, uint16_t len)
{
    if (!m_uart || !data || len == 0) return;
    HAL_UART_Transmit(m_uart,
                      reinterpret_cast<const uint8_t*>(data),
                      len, 1000);
}

uint16_t A7670C::readResponse(char* buf, uint16_t bsize, uint32_t timeout)
{
    if (!buf || bsize < 2) return 0;

    uint16_t idx   = 0;
    uint32_t start = HAL_GetTick();
    uint32_t lastByte = start;
    std::memset(buf, 0, bsize);

    while (idx < (uint16_t)(bsize - 1))
    {
        uint8_t ch;
        if (HAL_UART_Receive(m_uart, &ch, 1, 5) == HAL_OK) {
            buf[idx++] = static_cast<char>(ch);
            lastByte = HAL_GetTick();
            if (idx >= 4) {
                if (std::strstr(buf, "OK\r\n") ||
                    std::strstr(buf, "ERROR\r\n") ||
                    std::strstr(buf, "ERROR"))
                    break;
            }
        } else {
            uint32_t now = HAL_GetTick();
            if (idx > 0 && (now - lastByte) >= 30) break;
            if ((now - start) >= timeout) break;
        }
        IWDG->KR = 0xAAAA;
    }
    buf[idx] = '\0';
    return idx;
}

uint16_t A7670C::waitFor(char* buf, uint16_t bsize,
                          const char* expected, uint32_t timeout)
{
    if (!buf || !expected || bsize < 2) return 0;

    uint16_t idx   = 0;
    uint32_t start = HAL_GetTick();
    uint32_t lastByte = start;
    std::memset(buf, 0, bsize);

    while (idx < (uint16_t)(bsize - 1))
    {
        uint8_t ch;
        if (HAL_UART_Receive(m_uart, &ch, 1, 5) == HAL_OK) {
            buf[idx++] = static_cast<char>(ch);
            lastByte = HAL_GetTick();
            if (std::strstr(buf, expected) || std::strstr(buf, "ERROR"))
                break;
        } else {
            uint32_t now = HAL_GetTick();
            if (idx > 0 && (now - lastByte) >= 30) break;
            if ((now - start) >= timeout) break;
        }
        IWDG->KR = 0xAAAA;
    }
    buf[idx] = '\0';
    return idx;
}

GsmStatus A7670C::sendCommand(const char* cmd,
                               char*       resp,
                               uint16_t    rsize,
                               uint32_t    timeout)
{
    if (!resp || rsize == 0) return GsmStatus::Timeout;

    char at[256];
    std::snprintf(at, sizeof(at), "AT%s\r\n", cmd ? cmd : "");
    DBG.data("[A7670>] %s", at);

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
bool A7670C::waitRdy(uint32_t timeoutMs)
{
    char buf[256]{};
    uint16_t idx = 0;
    uint32_t start = HAL_GetTick();

    // Ждём "ME PDN ACT" — последнее сообщение загрузки A7670C
    while ((HAL_GetTick() - start) < timeoutMs) {
        uint8_t ch;
        if (HAL_UART_Receive(m_uart, &ch, 1, 20) == HAL_OK) {
            if (idx < sizeof(buf) - 1) buf[idx++] = static_cast<char>(ch);
            buf[idx] = '\0';
            if (std::strstr(buf, "ME PDN ACT")) {
                DBG.info("A7670C: загрузка завершена за %lu мс",
                         (unsigned long)(HAL_GetTick() - start));
                HAL_Delay(2000);  // пауза после последнего URC
                return true;
            }
            // Сдвигаем буфер если переполнен
            if (idx >= sizeof(buf) - 2) {
                std::memmove(buf, buf + 64, sizeof(buf) - 64);
                idx -= 64;
            }
        }
        IWDG->KR = 0xAAAA;
    }
    DBG.warn("A7670C: таймаут ожидания ME PDN ACT");
    return false;
}

void A7670C::powerOn()
{
    DBG.info("A7670C: включение питания...");
    HAL_GPIO_WritePin(m_pwrPort, m_pwrPin, GPIO_PIN_SET);
    HAL_Delay(300);

    // Проверить STATUS — если уже включён, пропустить PWRKEY
    GPIO_PinState status = HAL_GPIO_ReadPin(PIN_CELL_STATUS_PORT, PIN_CELL_STATUS_PIN);
    if (status == GPIO_PIN_SET) {
        DBG.info("A7670C: уже включён (STATUS=HIGH)");
        return;
    }

    DBG.info("A7670C: подача PWRKEY (1500 мс)...");
    HAL_GPIO_WritePin(PIN_CELL_PWRKEY_PORT, PIN_CELL_PWRKEY_PIN, GPIO_PIN_RESET);
    HAL_Delay(1500);   // A7670C требует ≥ 1000 мс, 1500 надёжнее
    HAL_GPIO_WritePin(PIN_CELL_PWRKEY_PORT, PIN_CELL_PWRKEY_PIN, GPIO_PIN_SET);

    DBG.info("A7670C: ожидание готовности...");
    if (!waitRdy(25000)) {
        DBG.warn("A7670C: ME PDN ACT не получен — пробуем AT");
    }
}

void A7670C::powerOff()
{
    DBG.info("A7670C: выключение...");
    GPIO_PinState status = HAL_GPIO_ReadPin(PIN_CELL_STATUS_PORT, PIN_CELL_STATUS_PIN);
    if (status == GPIO_PIN_RESET) {
        DBG.info("A7670C: уже выключен");
        HAL_GPIO_WritePin(m_pwrPort, m_pwrPin, GPIO_PIN_RESET);
        return;
    }
    char r[64];
    sendCommand("+CPOWD=1", r, sizeof(r), 3000);
    HAL_Delay(2000);
    HAL_GPIO_WritePin(m_pwrPort, m_pwrPin, GPIO_PIN_RESET);
}

void A7670C::hardReset()
{
    DBG.warn("A7670C: аппаратный сброс");
    HAL_GPIO_WritePin(PIN_CELL_RESET_PORT, PIN_CELL_RESET_PIN, GPIO_PIN_RESET);
    HAL_Delay(200);
    HAL_GPIO_WritePin(PIN_CELL_RESET_PORT, PIN_CELL_RESET_PIN, GPIO_PIN_SET);
    HAL_Delay(500);
    waitRdy(25000);
}

// ============================================================================
// Инициализация
// ============================================================================
GsmStatus A7670C::activatePdn()
{
    const RuntimeConfig& c = Cfg();
    char r[256], cmd[128];

    std::snprintf(cmd, sizeof(cmd),
                  "+CGDCONT=1,\"IP\",\"%s\"", c.gsm_apn);
    sendCommand(cmd, r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS);

    if (sendCommand("+CGACT=1,1", r, sizeof(r),
                    Config::SIM7020_PDN_TIMEOUT_MS) != GsmStatus::Ok) {
        // Контекст может быть уже активен — проверяем
        sendCommand("+CGACT?", r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS);
        if (!std::strstr(r, ",1")) {
            DBG.error("A7670C: PDN activation failed");
            return GsmStatus::PdnErr;
        }
    }

    sendCommand("+CGCONTRDP", r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS);
    DBG.info("A7670C: PDN UP — %s", r);
    return GsmStatus::Ok;
}

GsmStatus A7670C::init()
{
    char r[256];

    __HAL_UART_FLUSH_DRREGISTER(m_uart);
    HAL_Delay(100);

    // Синхронизация autobaud — несколько попыток AT
    DBG.info("A7670E: проверка связи...");
    bool alive = false;
    for (uint8_t i = 0; i < 10 && !alive; i++) {
        if (sendCommand("", r, sizeof(r), 2000) == GsmStatus::Ok) alive = true;
        else HAL_Delay(500);
        IWDG->KR = 0xAAAA;
    }
    if (!alive) {
        DBG.error("A7670C: нет ответа на AT");
        return GsmStatus::Timeout;
    }

    // Выключить эхо
    sendCommand("E0", r, sizeof(r), 2000);
    sendCommand("E0", r, sizeof(r), 2000);

    // Зафиксировать скорость
    sendCommand("+IPR=115200", r, sizeof(r), 2000);
    sendCommand("&W", r, sizeof(r), 2000);

    // Информация о модуле
    sendCommand("I", r, sizeof(r), 2000);
    DBG.info("A7670E:\n%s", r);

    // Версия прошивки
    sendCommand("+GMR", r, sizeof(r), 2000);
    DBG.info("A7670E FW:\n%s", r);

    // Авто-время из сети
    sendCommand("+CTZU=1", r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS);
    DBG.info("A7670E: CTZU (авто-время) включён");

    // SIM
    sendCommand("+CPIN?", r, sizeof(r), 5000);
    if (!std::strstr(r, "READY")) {
        DBG.error("A7670C: SIM не готова");
        return GsmStatus::NoSim;
    }
    DBG.info("A7670E: SIM OK");

    // Регистрация: сначала GSM (CREG), потом LTE (CEREG)
    // A7670C поддерживает fallback GSM/GPRS если LTE нет
    sendCommand("+CREG?", r, sizeof(r), 2000);
    bool registered = std::strstr(r, ",1") || std::strstr(r, ",5");
    DBG.info("A7670E: сеть OK");

    if (!registered) {
        sendCommand("+CEREG?", r, sizeof(r), 2000);
        registered = std::strstr(r, ",1") || std::strstr(r, ",5");
    }

    if (!registered) {
        // Ждём регистрацию до 60 сек
        for (uint8_t i = 0; i < 60 && !registered; i++) {
            sendCommand("+CREG?", r, sizeof(r), 2000);
            if (std::strstr(r, ",1") || std::strstr(r, ",5")) {
                registered = true; break;
            }
            sendCommand("+CEREG?", r, sizeof(r), 2000);
            if (std::strstr(r, ",1") || std::strstr(r, ",5")) {
                registered = true; break;
            }
            HAL_Delay(1000);
            IWDG->KR = 0xAAAA;
        }
    }

    if (!registered) {
        DBG.error("A7670C: нет регистрации в сети");
        return GsmStatus::NoReg;
    }

    // APN
    sendCommand("+CGDCONT=1,\"IP\",\"\"", r, sizeof(r),
                Config::SIM7020_CMD_TIMEOUT_MS);
    DBG.info("A7670E: APN настроен");

    sendCommand("+CGACT=1,1", r, sizeof(r), Config::SIM7020_PDN_TIMEOUT_MS);

    // CEREG для отладки
    sendCommand("+CEREG?", r, sizeof(r), 2000);
    DBG.info("A7670E: CEREG=%s", r);

    DBG.info("A7670E: APN настроен");
    DBG.info("A7670E: init завершён");
    return GsmStatus::Ok;
}

void A7670C::disconnect()
{
    char r[64];
    sendCommand("+CSOCL=0", r, sizeof(r), 2000);
    sendCommand("+CGACT=0,1", r, sizeof(r), 5000);
    DBG.info("A7670C: отключён");
}

// ============================================================================
// Утилиты
// ============================================================================
uint8_t A7670C::getSignalQuality()
{
    char r[64];
    sendCommand("+CSQ", r, sizeof(r), 2000);
    const char* p = std::strstr(r, "+CSQ:");
    if (p) {
        uint8_t v = 99;
        std::sscanf(p, "+CSQ: %hhu", &v);
        return v;
    }
    return 99;
}

// ============================================================================
// HTTP POST через TCP socket (AT+CSOC) — порт 80
// Отличие от SIM7020C: AT+CSOSEND вместо AT+CSODSEND
// ============================================================================
uint16_t A7670C::httpPost(const char* url, const char* json, uint16_t len)
{
    if (!url || !json || len == 0) return 0;

    const RuntimeConfig& c = Cfg();

    UrlParts u{};
    if (!parseHttpUrl(url, u)) {
        DBG.error("A7670C HTTP: не удалось разобрать URL: %s", url);
        return 0;
    }

    char r[512], cmd[256];

    // Создать TCP-сокет
    if (sendCommand("+CSOC=1,1,1", r, sizeof(r),
                    Config::SIM7020_CMD_TIMEOUT_MS) != GsmStatus::Ok) {
        DBG.error("A7670C HTTP: CSOC create failed");
        return 0;
    }

    uint8_t sockId = HTTP_SOCK_IDX;
    {
        const char* p = std::strstr(r, "+CSOC:");
        if (p) std::sscanf(p, "+CSOC: %hhu", &sockId);
    }

    // Подключиться к серверу
    std::snprintf(cmd, sizeof(cmd),
                  "+CSOCON=%hhu,%u,\"%s\"", sockId, u.port, u.host);
    if (sendCommand(cmd, r, sizeof(r),
                    Config::SIM7020_TCP_TIMEOUT_MS) != GsmStatus::Ok) {
        DBG.error("A7670C HTTP: CSOCON failed host=%s:%u", u.host, u.port);
        sendCommand("+CSOCL=0", r, sizeof(r), 2000);
        return 0;
    }

    // Сформировать HTTP-заголовок
    char hdr[600];
    int hdrLen;
    if (c.server_auth_b64[0]) {
        hdrLen = std::snprintf(hdr, sizeof(hdr),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: Basic %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n",
            u.path, u.host, c.server_auth_b64, (unsigned)len);
    } else {
        hdrLen = std::snprintf(hdr, sizeof(hdr),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n",
            u.path, u.host, (unsigned)len);
    }

    uint16_t totalLen = (uint16_t)(hdrLen + len);

    // AT+CSOSEND=<id>,<totalLen>,"<hex_data>"
    // A7670C использует HEX-строку в кавычках, не бинарный поток
    // Для простоты — отправляем заголовок и тело двумя CSOSEND
    std::snprintf(cmd, sizeof(cmd), "+CSOSEND=%hhu,%d", sockId, hdrLen);
    if (sendCommand(cmd, r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS) != GsmStatus::Ok) {
        DBG.error("A7670C HTTP: CSOSEND header failed");
        sendCommand("+CSOCL=0", r, sizeof(r), 2000);
        return 0;
    }
    // Ждём '>' и отправляем данные
    sendRaw(hdr, (uint16_t)hdrLen);
    readResponse(r, sizeof(r), 3000);

    std::snprintf(cmd, sizeof(cmd), "+CSOSEND=%hhu,%u", sockId, (unsigned)len);
    if (sendCommand(cmd, r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS) != GsmStatus::Ok) {
        DBG.error("A7670C HTTP: CSOSEND body failed");
        sendCommand("+CSOCL=0", r, sizeof(r), 2000);
        return 0;
    }
    sendRaw(json, len);
    readResponse(r, sizeof(r), 3000);

    // Прочитать HTTP-ответ
    std::snprintf(cmd, sizeof(cmd), "+CSORCVDATA=%hhu,512", sockId);
    waitFor(r, sizeof(r), "HTTP/1.", Config::SIM7020_TCP_TIMEOUT_MS);

    uint16_t code = 0;
    const char* p = std::strstr(r, "HTTP/1.");
    if (p) std::sscanf(p, "HTTP/1.%*c %hu", &code);
    DBG.info("A7670C HTTP: response code %u", (unsigned)code);

    sendCommand("+CSOCL=0", r, sizeof(r), 2000);
    return code;
}

// ============================================================================
// MQTT (нативный стек A7670C)
// ============================================================================
GsmStatus A7670C::mqttConnect(const char* broker, uint16_t port,
                               const char* clientId,
                               const char* user, const char* pass)
{
    char r[256], cmd[256];

    // Создать MQTT клиент
    std::snprintf(cmd, sizeof(cmd),
        "+CMQNEW=\"%s\",%u,10000,1024", broker, port);
    if (sendCommand(cmd, r, sizeof(r), 15000) != GsmStatus::Ok) {
        DBG.error("A7670C MQTT: CMQNEW failed");
        return GsmStatus::MqttErr;
    }

    uint8_t mqttId = 0;
    {
        const char* p = std::strstr(r, "+CMQNEW:");
        if (p) std::sscanf(p, "+CMQNEW: %hhu", &mqttId);
    }

    // Подключиться
    if (user && user[0]) {
        std::snprintf(cmd, sizeof(cmd),
            "+CMQCON=%hhu,3,\"%s\",60,1,0,\"%s\",\"%s\"",
            mqttId, clientId, user, pass ? pass : "");
    } else {
        std::snprintf(cmd, sizeof(cmd),
            "+CMQCON=%hhu,3,\"%s\",60,0",
            mqttId, clientId);
    }

    if (sendCommand(cmd, r, sizeof(r), 15000) != GsmStatus::Ok) {
        DBG.error("A7670C MQTT: CMQCON failed");
        return GsmStatus::MqttErr;
    }

    DBG.info("A7670C MQTT: подключён к %s:%u", broker, port);
    return GsmStatus::Ok;
}

GsmStatus A7670C::mqttPublish(const char* topic, const char* payload, uint16_t len)
{
    char r[256], cmd[512];
    std::snprintf(cmd, sizeof(cmd),
        "+CMQPUB=0,\"%s\",1,0,0,%u,\"%.*s\"",
        topic, (unsigned)len, (int)len, payload);

    if (sendCommand(cmd, r, sizeof(r), 10000) != GsmStatus::Ok) {
        DBG.error("A7670C MQTT: CMQPUB failed");
        return GsmStatus::MqttErr;
    }
    return GsmStatus::Ok;
}

void A7670C::mqttDisconnect()
{
    char r[64];
    sendCommand("+CMQDISCON=0", r, sizeof(r), 5000);
    DBG.info("A7670C MQTT: отключён");
}
