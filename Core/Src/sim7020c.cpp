/**
 * ================================================================
 * @file    sim7020c.cpp
 * @brief   Реализация драйвера SIM7020C NB-IoT (замена SIM800L).
 *
 * Протоколы:
 *   HTTP POST — через нативный TCP socket (AT+CSOC/CSOCON/CSOSEND/CSORCV)
 *   MQTT      — через нативный MQTT-стек (AT+CMQNEW/CMQCON/CMQPUB)
 *
 * AT-команды: SIM7020 Series AT Command Manual v1.03
 * ================================================================
 */
#include "sim7020c.hpp"
#include "debug_uart.hpp"
#include "board_pins.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

// ============================================================================
// Вспомогательная функция парсинга URL
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

    // host
    const char* hostBeg = p;
    while (*p && *p != '/' && *p != ':') p++;
    size_t hostLen = (size_t)(p - hostBeg);
    if (hostLen == 0 || hostLen >= sizeof(out.host)) return false;
    std::memcpy(out.host, hostBeg, hostLen);
    out.host[hostLen] = '\0';

    // порт
    if (*p == ':') {
        p++;
        uint32_t port = 0;
        while (*p && std::isdigit((unsigned char)*p)) {
            port = port * 10u + (uint32_t)(*p++ - '0');
        }
        if (port == 0 || port > 65535) return false;
        out.port = (uint16_t)port;
    }

    // путь
    if (*p == '\0') {
        std::strcpy(out.path, "/");
        return true;
    }
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
SIM7020C::SIM7020C(UART_HandleTypeDef* uart,
                   GPIO_TypeDef*        pwrPort,
                   uint16_t             pwrPin)
    : m_uart(uart), m_pwrPort(pwrPort), m_pwrPin(pwrPin)
{}

// ============================================================================
// Низкоуровневый I/O
// ============================================================================
void SIM7020C::sendRaw(const char* data, uint16_t len)
{
    if (!m_uart || !data || len == 0) return;
    HAL_UART_Transmit(m_uart,
                      reinterpret_cast<const uint8_t*>(data),
                      len,
                      1000);
}

uint16_t SIM7020C::readResponse(char* buf, uint16_t bsize, uint32_t timeout)
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
            // Нет байта 5 мс — проверяем условия выхода
            uint32_t now = HAL_GetTick();

            // Если уже получили данные и тишина >30 мс — конец ответа
            if (idx > 0 && (now - lastByte) >= 30) break;

            // Общий таймаут
            if ((now - start) >= timeout) break;
        }
        IWDG->KR = 0xAAAA;
    }

    buf[idx] = '\0';
    return idx;
}

uint16_t SIM7020C::waitFor(char* buf, uint16_t bsize,
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

            if (std::strstr(buf, expected) ||
                std::strstr(buf, "ERROR"))
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
GsmStatus SIM7020C::sendCommand(const char* cmd,
                                 char*       resp,
                                 uint16_t    rsize,
                                 uint32_t    timeout)
{
    if (!resp || rsize == 0) return GsmStatus::Timeout;

    char at[256];
    std::snprintf(at, sizeof(at), "AT%s\r\n", cmd ? cmd : "");
    DBG.data("[SIM>] %s", at);

    sendRaw(at, (uint16_t)std::strlen(at));
    readResponse(resp, rsize, timeout);

    DBG.data("[SIM<] %s", resp);

    if (std::strstr(resp, "OK"))    return GsmStatus::Ok;
    if (std::strstr(resp, "ERROR")) return GsmStatus::HttpErr;
    return GsmStatus::Timeout;
}

// ============================================================================
// Управление питанием
// ============================================================================
bool SIM7020C::waitRdy(uint32_t timeoutMs)
{
    char buf[128]{};
    uint16_t idx = 0;
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeoutMs) {
        uint8_t ch;
        if (HAL_UART_Receive(m_uart, &ch, 1, 20) == HAL_OK) {
            if (idx < sizeof(buf) - 1) buf[idx++] = static_cast<char>(ch);
            if (std::strstr(buf, "RDY")) {
                DBG.info("SIM7020C: RDY получен за %lu мс",
                         (unsigned long)(HAL_GetTick() - start));
                return true;
            }
        }
        IWDG->KR = 0xAAAA;
    }
    return false;
}

void SIM7020C::powerOn() {
    DBG.info("SIM7020C: включение питания...");
    HAL_GPIO_WritePin(m_pwrPort, m_pwrPin, GPIO_PIN_SET);
    HAL_Delay(300);

    // Если модем уже включён (VDD_EXT = HIGH) — не трогать PWRKEY!
    GPIO_PinState status = HAL_GPIO_ReadPin(PIN_CELL_STATUS_PORT, PIN_CELL_STATUS_PIN);
    if (status == GPIO_PIN_SET) {
        DBG.info("SIM7020C: уже включён (STATUS=HIGH), PWRKEY пропускаем");
        return;
    }

    // Модем выключен — включаем
    DBG.info("SIM7020C: подача PWRKEY...");
    HAL_GPIO_WritePin(PIN_CELL_PWRKEY_PORT, PIN_CELL_PWRKEY_PIN, GPIO_PIN_RESET);
    HAL_Delay(1200);  // Air780E требует ≥ 1000 мс
    HAL_GPIO_WritePin(PIN_CELL_PWRKEY_PORT, PIN_CELL_PWRKEY_PIN, GPIO_PIN_SET);

    DBG.info("SIM7020C: ожидание RDY...");
    if (!waitRdy(12000)) {
        DBG.warn("SIM7020C: RDY не получен — продолжаем с таймаутом");
    }
}

void SIM7020C::powerOff() {
    DBG.info("SIM7020C: выключение...");
    GPIO_PinState status = HAL_GPIO_ReadPin(PIN_CELL_STATUS_PORT, PIN_CELL_STATUS_PIN);
    if (status == GPIO_PIN_RESET) {
        DBG.info("SIM7020C: уже выключен, пропускаем");
        HAL_GPIO_WritePin(m_pwrPort, m_pwrPin, GPIO_PIN_RESET);
        return;
    }
    char r[64];
    sendCommand("+CPOWD=1", r, sizeof(r), 3000);
    HAL_Delay(2000);
    HAL_GPIO_WritePin(m_pwrPort, m_pwrPin, GPIO_PIN_RESET);
}

void SIM7020C::hardReset()
{
    DBG.warn("SIM7020C: аппаратный сброс");
    HAL_GPIO_WritePin(PIN_CELL_RESET_PORT, PIN_CELL_RESET_PIN, GPIO_PIN_RESET);
    HAL_Delay(200);
    HAL_GPIO_WritePin(PIN_CELL_RESET_PORT, PIN_CELL_RESET_PIN, GPIO_PIN_SET);
    HAL_Delay(500);
    waitRdy(Config::SIM7020_BOOT_MS);
}

// ============================================================================
// Инициализация
// ============================================================================
GsmStatus SIM7020C::activatePdn()
{
    const RuntimeConfig& c = Cfg();
    char r[256], cmd[128];

    // Установить APN в профиль 1
    std::snprintf(cmd, sizeof(cmd),
                  "+CGDCONT=1,\"IP\",\"%s\"", c.gsm_apn);
    sendCommand(cmd, r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS);

    // Активировать контекст
    if (sendCommand("+CGACT=1,1", r, sizeof(r),
                    Config::SIM7020_PDN_TIMEOUT_MS) != GsmStatus::Ok) {
        DBG.error("SIM7020C: CGACT fail");
        return GsmStatus::PdnErr;
    }

    // Прочитать IP для отладки
    sendCommand("+CGCONTRDP", r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS);
    DBG.info("SIM7020C: PDN UP — %s", r);
    return GsmStatus::Ok;
}

GsmStatus SIM7020C::init()
{
	 char r[256];

	    // Сброс UART RX — очистить мусор накопившийся при старте модема
	    __HAL_UART_FLUSH_DRREGISTER(m_uart);
	    HAL_Delay(100);

	    DBG.info("SIM7020C: проверка связи...");
	    bool alive = false;
	    for (uint8_t i = 0; i < 10 && !alive; i++) {
	        if (sendCommand("", r, sizeof(r), 2000) == GsmStatus::Ok) alive = true;
	        else HAL_Delay(500);
	        IWDG->KR = 0xAAAA;
	    }
    if (!alive) {
        DBG.error("SIM7020C: нет ответа на AT");
        return GsmStatus::Timeout;
    }

    // Выключить эхо
    sendCommand("E0", r, sizeof(r), 2000);

    // Проверить SIM
    sendCommand("+CPIN?", r, sizeof(r), 5000);
    if (!std::strstr(r, "READY")) {
        DBG.error("SIM7020C: SIM не готова");
        return GsmStatus::NoSim;
    }
    DBG.info("SIM7020C: SIM OK");

    // Ожидать регистрацию в NB-IoT сети (AT+CEREG для NB-IoT)
    bool registered = false;
    for (uint8_t i = 0; i < 60 && !registered; i++) {
        sendCommand("+CEREG?", r, sizeof(r), 2000);
        // 1 = зарегистрирован (домашняя), 5 = роуминг
        if (std::strstr(r, ",1") || std::strstr(r, ",5")) {
            registered = true;
        } else {
            HAL_Delay(1000);
            IWDG->KR = 0xAAAA;
        }
    }
    if (!registered) {
        DBG.error("SIM7020C: нет регистрации в NB-IoT сети");
        return GsmStatus::NoReg;
    }

    DBG.info("SIM7020C: сеть OK, CSQ=%d", getSignalQuality());

    return activatePdn();
}

void SIM7020C::disconnect()
{
    char r[64];
    // Закрыть все сокеты
    sendCommand("+CSOCL=0", r, sizeof(r), 2000);
    // Деактивировать PDN
    sendCommand("+CGACT=0,1", r, sizeof(r), 5000);
    DBG.info("SIM7020C: отключён");
}

// ============================================================================
// Утилиты
// ============================================================================
uint8_t SIM7020C::getSignalQuality()
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
// HTTP POST через TCP socket (AT+CSOC)
// ============================================================================
uint16_t SIM7020C::httpPost(const char* url, const char* json, uint16_t len)
{
    if (!url || !json || len == 0) return 0;

    const RuntimeConfig& c = Cfg();

    UrlParts u{};
    if (!parseHttpUrl(url, u)) {
        DBG.error("SIM7020C HTTP: не удалось разобрать URL: %s", url);
        return 0;
    }

    char r[512], cmd[256];

    // Создать TCP-сокет: AT+CSOC=1,1,1 (domain=IPv4, type=TCP, protocol=TCP)
    if (sendCommand("+CSOC=1,1,1", r, sizeof(r),
                    Config::SIM7020_CMD_TIMEOUT_MS) != GsmStatus::Ok) {
        DBG.error("SIM7020C HTTP: CSOC create failed");
        return 0;
    }

    // Извлечь socket_id из "+CSOC: <id>"
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
        DBG.error("SIM7020C HTTP: CSOCON failed host=%s:%u", u.host, u.port);
        sendCommand("+CSOCL=0", r, sizeof(r), 2000);
        return 0;
    }

    // Сформировать HTTP-запрос
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

    if (hdrLen <= 0 || (size_t)hdrLen >= sizeof(hdr)) {
        DBG.error("SIM7020C HTTP: header overflow");
        sendCommand("+CSOCL=0", r, sizeof(r), 2000);
        return 0;
    }

    // Отправить заголовок: AT+CSOSEND=<id>,<len>,"<hex>"
    // SIM7020C принимает CSOSEND с текстом (не hex) через режим CSODSEND
    // Используем AT+CSODSEND=<id>,<total_len> + прямая передача данных
    uint16_t total = (uint16_t)hdrLen + len;
    std::snprintf(cmd, sizeof(cmd), "+CSODSEND=%hhu,%u", sockId, (unsigned)total);
    sendRaw("AT", 2);
    sendRaw(cmd + 1, (uint16_t)std::strlen(cmd) - 1);  // без первого +
    {
        // Правильно: отправить "AT+CSODSEND=id,len\r\n"
        char fullCmd[64];
        std::snprintf(fullCmd, sizeof(fullCmd),
                      "AT+CSODSEND=%hhu,%u\r\n", sockId, (unsigned)total);
        sendRaw(fullCmd, (uint16_t)std::strlen(fullCmd));
    }

    // Ждём ">" (prompt)
    waitFor(r, sizeof(r), ">", 3000);
    if (!std::strstr(r, ">")) {
        DBG.error("SIM7020C HTTP: нет prompt '>'");
        sendCommand("+CSOCL=0", r, sizeof(r), 2000);
        return 0;
    }

    // Отправить заголовок и тело запроса
    sendRaw(hdr, (uint16_t)hdrLen);
    sendRaw(json, len);

    // Ждём ответа сервера через URC "+CSORDATAIND" или "+CSORCV"
    waitFor(r, sizeof(r), "+CSORDATAIND", Config::SIM7020_TCP_TIMEOUT_MS);

    // Читаем ответ: AT+CSORCV=<id>,<len>
    std::snprintf(cmd, sizeof(cmd), "+CSORCV=%hhu,256", sockId);
    sendCommand(cmd, r, sizeof(r), Config::SIM7020_CMD_TIMEOUT_MS);

    // Разобрать HTTP-код
    uint16_t httpCode = 0;
    const char* httpPos = std::strstr(r, "HTTP/1.");
    if (httpPos) {
        int code = 0;
        if (std::sscanf(httpPos, "HTTP/%*s %d", &code) == 1) {
            httpCode = (uint16_t)code;
        }
    }

    DBG.info("SIM7020C HTTP: код ответа=%u", httpCode);

    // Закрыть сокет
    std::snprintf(cmd, sizeof(cmd), "+CSOCL=%hhu", sockId);
    sendCommand(cmd, r, sizeof(r), 2000);

    return httpCode;
}

// ============================================================================
// MQTT (нативный протокол SIM7020C)
// ============================================================================
GsmStatus SIM7020C::mqttConnect(const char* broker, uint16_t port)
{
    if (!broker) return GsmStatus::MqttErr;

    char r[512], cmd[256];

    // Закрыть предыдущую сессию, если была
    std::snprintf(cmd, sizeof(cmd), "+CMQDISCON=%hhu", MQTT_IDX);
    sendCommand(cmd, r, sizeof(r), 3000);
    HAL_Delay(200);

    // Создать новую MQTT-сессию (keepalive=120, cleanSession=1, recvBuf=1024)
    // AT+CMQNEW=<broker>,<port>,<timeout_ms>,<buf_size>
    std::snprintf(cmd, sizeof(cmd),
                  "+CMQNEW=\"%s\",\"%u\",%lu,1024",
                  broker, port,
                  (unsigned long)Config::SIM7020_MQTT_TIMEOUT_MS);
    if (sendCommand(cmd, r, sizeof(r),
                    Config::SIM7020_MQTT_TIMEOUT_MS + 2000) != GsmStatus::Ok) {
        DBG.error("SIM7020C MQTT: CMQNEW failed");
        return GsmStatus::MqttErr;
    }

    // Формируем clientId из последних 4 байт MAC или фиксированный
    // AT+CMQCON=<idx>,<version>,<clientId>,<keepalive>,<cleanSession>,<willFlag>
    std::snprintf(cmd, sizeof(cmd),
                  "+CMQCON=%hhu,3,\"MonSet_%04X\",120,1,0",
                  MQTT_IDX,
                  (unsigned)(HAL_GetTick() & 0xFFFFu));

    if (sendCommand(cmd, r, sizeof(r),
                    Config::SIM7020_MQTT_TIMEOUT_MS) != GsmStatus::Ok) {
        DBG.error("SIM7020C MQTT: CMQCON failed");
        return GsmStatus::MqttErr;
    }

    DBG.info("SIM7020C MQTT: подключён к %s:%u", broker, port);
    return GsmStatus::Ok;
}

GsmStatus SIM7020C::mqttPublish(const char* topic,
                                 const char* payload,
                                 uint8_t     qos)
{
    if (!topic || !payload) return GsmStatus::MqttErr;

    uint16_t payLen = (uint16_t)std::strlen(payload);
    char r[256], cmd[128];

    // AT+CMQPUB=<idx>,<topic>,<qos>,<retain>,<dup>,<msgLen>,<msg>
    std::snprintf(cmd, sizeof(cmd),
                  "+CMQPUB=%hhu,\"%s\",%hhu,0,0,%u,",
                  MQTT_IDX, topic, qos, payLen);

    // Полная команда с payload
    char fullCmd[512];
    int cmdLen = std::snprintf(fullCmd, sizeof(fullCmd), "AT%s\"%s\"\r\n",
                                cmd, payload);
    if (cmdLen <= 0 || (size_t)cmdLen >= sizeof(fullCmd)) {
        DBG.error("SIM7020C MQTT: publish command overflow");
        return GsmStatus::MqttErr;
    }

    DBG.data("[SIM>] %s", fullCmd);
    sendRaw(fullCmd, (uint16_t)cmdLen);
    readResponse(r, sizeof(r), Config::SIM7020_MQTT_TIMEOUT_MS);
    DBG.data("[SIM<] %s", r);

    if (std::strstr(r, "OK")) {
        DBG.info("SIM7020C MQTT: publish OK topic=%s", topic);
        return GsmStatus::Ok;
    }

    DBG.error("SIM7020C MQTT: publish FAILED topic=%s resp=%s", topic, r);
    return GsmStatus::MqttErr;
}

void SIM7020C::mqttDisconnect()
{
    char r[64], cmd[32];
    std::snprintf(cmd, sizeof(cmd), "+CMQDISCON=%hhu", MQTT_IDX);
    sendCommand(cmd, r, sizeof(r), 3000);
    DBG.info("SIM7020C MQTT: отключён");
}
