/**
 * ================================================================
 * @file    sim7020c_tls.cpp
 * @brief   TLS-туннель поверх SIM7020C TCP-сокета.
 *
 * Транспортный слой:
 *   mbedTLS вызывает biosend/biorecv, которые отправляют/читают
 *   данные через AT+CSODSEND / AT+CSORCV (SIM7020C TCP socket).
 *
 *   mbedTLS НЕ знает, что снизу — UART + AT-команды. Для него
 *   это просто stream-транспорт с произвольными send/recv.
 *
 * Важные ограничения SIM7020C:
 *   - AT+CSODSEND=<id>,<len> принимает бинарный поток N байт
 *     (не HEX, не строку). Это критично для TLS — хендшейк
 *     передаёт двоичные сертификаты и ключи.
 *   - AT+CSORCV=<id>,<maxLen> возвращает данные в HEX или бинаре
 *     в зависимости от AT+CREVHEX. Мы используем бинарный режим
 *     (AT+CREVHEX=0 по умолчанию).
 *   - Максимальный размер одного CSODSEND — 1024 байта (datasheet).
 *   - Максимальный размер одного CSORCV  — 1024 байта.
 *
 * Память:
 *   mbedTLS требует ~40-80 КБ RAM для TLS 1.2. STM32F407 имеет
 *   192 КБ RAM — достаточно при правильной конфигурации.
 *   Оптимизируйте через MBEDTLS_SSL_MAX_CONTENT_LEN = 4096 если нужно.
 * ================================================================
 */
#include "sim7020c_tls.hpp"
#include "debug_uart.hpp"

extern "C" {
#include "mbedtls/net_sockets.h"  // только для кодов ошибок MBEDTLS_ERR_NET_*
#include "mbedtls/error.h"
}

#include <cstdio>
#include <cstring>
#include <cctype>

// ============================================================================
// Константы
// ============================================================================
static constexpr uint16_t MODEM_MAX_SEND = 512;  ///< Макс. байт за один CSODSEND
static constexpr uint16_t MODEM_MAX_RECV = 512;  ///< Макс. байт за один CSORCV
static constexpr uint32_t BYTE_TIMEOUT   = 200;  ///< Таймаут ожидания одного байта

// ============================================================================
// Конструктор
// ============================================================================
Sim7020cTls::Sim7020cTls(SIM7020C& modem, uint32_t timeoutMs)
    : m_modem(modem), m_timeoutMs(timeoutMs)
{
    mbedtls_ssl_init(&m_ssl);
    mbedtls_ssl_config_init(&m_conf);
    mbedtls_entropy_init(&m_entropy);
    mbedtls_ctr_drbg_init(&m_ctr);
    mbedtls_x509_crt_init(&m_cacert);
    m_tlsInited = true;
}

// ============================================================================
// Вспомогательные
// ============================================================================
void Sim7020cTls::logMbedtlsErr(const char* tag, int rc)
{
    char buf[128];
    std::memset(buf, 0, sizeof(buf));
    mbedtls_strerror(rc, buf, sizeof(buf));
    DBG.error("%s rc=-0x%04X (%s)", tag, (unsigned)(-rc), buf[0] ? buf : "?");
}

void Sim7020cTls::tlsFree()
{
    if (!m_tlsInited) return;
    mbedtls_ssl_free(&m_ssl);
    mbedtls_ssl_config_free(&m_conf);
    mbedtls_x509_crt_free(&m_cacert);
    mbedtls_ctr_drbg_free(&m_ctr);
    mbedtls_entropy_free(&m_entropy);

    // Реинициализируем для возможного повторного использования
    mbedtls_ssl_init(&m_ssl);
    mbedtls_ssl_config_init(&m_conf);
    mbedtls_entropy_init(&m_entropy);
    mbedtls_ctr_drbg_init(&m_ctr);
    mbedtls_x509_crt_init(&m_cacert);
}

// ============================================================================
// Разбор HTTPS URL
// ============================================================================
bool Sim7020cTls::parseHttpsUrl(const char* url, UrlParts& out)
{
    if (!url) return false;
    out = UrlParts{};
    out.port = 443;

    const char* prefix = "https://";
    size_t prefixLen = std::strlen(prefix);
    if (std::strncmp(url, prefix, prefixLen) != 0) return false;

    const char* p = url + prefixLen;

    // Host
    const char* hb = p;
    while (*p && *p != '/' && *p != ':') p++;
    size_t hl = (size_t)(p - hb);
    if (hl == 0 || hl >= sizeof(out.host)) return false;
    std::memcpy(out.host, hb, hl);
    out.host[hl] = '\0';

    // Port
    if (*p == ':') {
        p++;
        uint32_t port = 0;
        while (*p && std::isdigit((unsigned char)*p)) {
            port = port * 10u + (uint32_t)(*p++ - '0');
        }
        if (port == 0 || port > 65535) return false;
        out.port = (uint16_t)port;
    }

    // Path
    if (*p == '\0') { std::strcpy(out.path, "/"); return true; }
    if (*p != '/')  return false;
    size_t pl = std::strlen(p);
    if (pl == 0 || pl >= sizeof(out.path)) return false;
    std::memcpy(out.path, p, pl + 1);
    return true;
}

// ============================================================================
// Транспортный слой: отправка через AT+CSODSEND
// ============================================================================
int Sim7020cTls::modemWriteRaw(const uint8_t* buf, uint16_t len)
{
    if (!buf || len == 0) return 0;

    uint16_t sent = 0;
    char cmd[64];
    char r[64];

    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > MODEM_MAX_SEND) chunk = MODEM_MAX_SEND;

        // Отправить AT+CSODSEND=<id>,<len>\r\n
        std::snprintf(cmd, sizeof(cmd),
                      "AT+CSODSEND=%hhu,%u\r\n", m_sockId, (unsigned)chunk);
        m_modem.sendRaw_pub(cmd, (uint16_t)std::strlen(cmd));

        // Ждём символ '>' (prompt)
        m_modem.waitFor_pub(r, sizeof(r), ">", 3000);
        if (!std::strstr(r, ">")) {
            DBG.error("TLS BIO: нет prompt '>' для CSODSEND");
            return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        }

        // Отправляем бинарные данные прямо в UART
        m_modem.sendRaw_pub(reinterpret_cast<const char*>(buf + sent), chunk);

        // Ждём OK
        m_modem.waitFor_pub(r, sizeof(r), "OK", 3000);
        if (!std::strstr(r, "OK")) {
            DBG.warn("TLS BIO: CSODSEND не ответил OK (chunk=%u)", (unsigned)chunk);
        }

        sent += chunk;
        IWDG->KR = 0xAAAA;
    }

    return (int)sent;
}

// ============================================================================
// Транспортный слой: приём через AT+CSORCV
// ============================================================================
int Sim7020cTls::modemReadRaw(uint8_t* buf, uint16_t len)
{
    if (!buf || len == 0) return 0;

    uint16_t want = (len > MODEM_MAX_RECV) ? MODEM_MAX_RECV : len;
    char cmd[64];
    char r[MODEM_MAX_RECV + 128];

    // AT+CSORCV=<id>,<maxLen>
    std::snprintf(cmd, sizeof(cmd),
                  "AT+CSORCV=%hhu,%u\r\n", m_sockId, (unsigned)want);
    m_modem.sendRaw_pub(cmd, (uint16_t)std::strlen(cmd));

    // Ответ формата:
    //   +CSORCV: <id>,<recvLen>\r\n<binaryData>\r\nOK
    uint16_t rxLen = m_modem.waitFor_pub(r, sizeof(r), "+CSORCV:", 3000);
    (void)rxLen;

    if (!std::strstr(r, "+CSORCV:")) {
        // Нет данных — сообщаем mbedTLS "хочу ещё читать"
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    // Разобрать: +CSORCV: <id>,<recvLen>
    const char* p = std::strstr(r, "+CSORCV:");
    int id = 0, recvLen = 0;
    if (std::sscanf(p, "+CSORCV: %d,%d", &id, &recvLen) != 2 || recvLen <= 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    // Данные идут сразу после "\r\n" после заголовка
    const char* dataStart = std::strchr(p, '\n');
    if (!dataStart) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    dataStart++;  // пропустить '\n'

    uint16_t copy = (uint16_t)recvLen;
    if (copy > len) copy = (uint16_t)len;

    // Данные могут быть бинарные — копируем recvLen байт
    size_t available = rxLen - (size_t)(dataStart - r);
    if (available < copy) copy = (uint16_t)available;

    std::memcpy(buf, dataStart, copy);

    IWDG->KR = 0xAAAA;
    return (int)copy;
}

// ============================================================================
// mbedTLS BIO callbacks (static методы — нет this, ctx = this)
// ============================================================================
int Sim7020cTls::biosend(void* ctx, const unsigned char* buf, size_t len)
{
    auto* self = static_cast<Sim7020cTls*>(ctx);
    if (!self || !buf || len == 0) return 0;

    // Проверяем дедлайн
    if (HAL_GetTick() > self->m_deadline) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }

    uint16_t toSend = (len > MODEM_MAX_SEND) ? MODEM_MAX_SEND : (uint16_t)len;
    int r = self->modemWriteRaw(buf, toSend);
    if (r <= 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return r;
}

int Sim7020cTls::biorecv(void* ctx, unsigned char* buf, size_t len)
{
    auto* self = static_cast<Sim7020cTls*>(ctx);
    if (!self || !buf || len == 0) return 0;

    if (HAL_GetTick() > self->m_deadline) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }

    uint16_t toRead = (len > MODEM_MAX_RECV) ? MODEM_MAX_RECV : (uint16_t)len;
    int r = self->modemReadRaw(buf, toRead);

    // WANT_READ — нормально, mbedTLS повторит
    if (r == MBEDTLS_ERR_SSL_WANT_READ) return r;
    if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (r == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    return r;
}

// ============================================================================
// connect(): TCP + TLS handshake
// ============================================================================
int Sim7020cTls::connect(const char* host, uint16_t port)
{
    DBG.info("TLS: connect %s:%u", host, port);

    m_deadline = HAL_GetTick() + m_timeoutMs;

    // ---- Открыть TCP-сокет через SIM7020C ----
    char r[256], cmd[128];

    // Убедиться что предыдущий сокет закрыт
    std::snprintf(cmd, sizeof(cmd), "AT+CSOCL=%hhu\r\n", m_sockId);
    m_modem.sendRaw_pub(cmd, (uint16_t)std::strlen(cmd));
    HAL_Delay(100);

    // Создать TCP-сокет
    m_modem.sendRaw_pub("AT+CSOC=1,1,1\r\n", 15);
    m_modem.waitFor_pub(r, sizeof(r), "+CSOC:", 3000);
    if (!std::strstr(r, "+CSOC:")) {
        // Попробуем "OK" — некоторые прошивки возвращают только ID без +CSOC:
        if (!std::strstr(r, "OK")) {
            DBG.error("TLS: CSOC create failed");
            return -1;
        }
    }
    // Парсим socket id
    const char* sp = std::strstr(r, "+CSOC:");
    if (sp) std::sscanf(sp, "+CSOC: %hhu", &m_sockId);

    // Подключиться к серверу
    std::snprintf(cmd, sizeof(cmd),
                  "AT+CSOCON=%hhu,%u,\"%s\"\r\n", m_sockId, port, host);
    m_modem.sendRaw_pub(cmd, (uint16_t)std::strlen(cmd));
    m_modem.waitFor_pub(r, sizeof(r), "OK", 15000);
    if (!std::strstr(r, "OK")) {
        DBG.error("TLS: CSOCON failed host=%s:%u", host, port);
        return -2;
    }
    DBG.info("TLS: TCP socket open OK (id=%hhu)", m_sockId);

    // ---- mbedTLS: seed ----
    const char* pers = "sim7020c_tls";
    int rc = mbedtls_ctr_drbg_seed(&m_ctr,
                                    mbedtls_entropy_func,
                                    &m_entropy,
                                    reinterpret_cast<const unsigned char*>(pers),
                                    std::strlen(pers));
    if (rc != 0) {
        logMbedtlsErr("TLS: ctr_drbg_seed", rc);
        close();
        return -10;
    }

    // ---- CA cert ----
    if (m_caPem) {
        rc = mbedtls_x509_crt_parse(&m_cacert,
                                     reinterpret_cast<const unsigned char*>(m_caPem),
                                     std::strlen(m_caPem) + 1);
        if (rc < 0) {
            logMbedtlsErr("TLS: x509_crt_parse", rc);
            close();
            return -11;
        }
    }

    // ---- SSL config ----
    rc = mbedtls_ssl_config_defaults(&m_conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        logMbedtlsErr("TLS: config_defaults", rc);
        close();
        return -12;
    }

    mbedtls_ssl_conf_rng(&m_conf, mbedtls_ctr_drbg_random, &m_ctr);

    if (m_caPem) {
        mbedtls_ssl_conf_authmode(&m_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&m_conf, &m_cacert, nullptr);
    } else {
        // VERIFY_NONE: не проверяем сертификат сервера.
        // Достаточно для защиты от перехвата (данные зашифрованы),
        // но не защищает от MITM с поддельным сертификатом.
        // Для промышленного применения включите VERIFY_REQUIRED + CA.
        mbedtls_ssl_conf_authmode(&m_conf, MBEDTLS_SSL_VERIFY_NONE);
        DBG.warn("TLS: VERIFY_NONE (CA cert не задан)");
    }

    // ---- SSL setup ----
    rc = mbedtls_ssl_setup(&m_ssl, &m_conf);
    if (rc != 0) {
        logMbedtlsErr("TLS: ssl_setup", rc);
        close();
        return -13;
    }

    // SNI: имя хоста для Server Name Indication
    rc = mbedtls_ssl_set_hostname(&m_ssl, host);
    if (rc != 0) {
        logMbedtlsErr("TLS: set_hostname", rc);
        close();
        return -14;
    }

    // Подключить наши BIO callbacks
    mbedtls_ssl_set_bio(&m_ssl,
                        this,
                        Sim7020cTls::biosend,
                        Sim7020cTls::biorecv,
                        nullptr);

    // ---- TLS handshake ----
    DBG.info("TLS: handshake start...");
    while (true) {
        rc = mbedtls_ssl_handshake(&m_ssl);

        if (rc == 0) break;

        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (HAL_GetTick() > m_deadline) {
                DBG.error("TLS: handshake timeout");
                close();
                return -20;
            }
            HAL_Delay(5);
            IWDG->KR = 0xAAAA;
            continue;
        }

        logMbedtlsErr("TLS: handshake", rc);
        close();
        return -21;
    }

    DBG.info("TLS: handshake OK, cipher=%s",
             mbedtls_ssl_get_ciphersuite(&m_ssl));
    m_connected = true;
    return 0;
}

// ============================================================================
// write() / read() — публичный API для тех, кто хочет работать напрямую
// ============================================================================
int Sim7020cTls::write(const uint8_t* buf, size_t len)
{
    if (!m_connected) return -1;

    size_t off = 0;
    while (off < len) {
        int w = mbedtls_ssl_write(&m_ssl, buf + off, len - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) {
            HAL_Delay(2);
            IWDG->KR = 0xAAAA;
            continue;
        }
        logMbedtlsErr("TLS: write", w);
        return -1;
    }
    return (int)off;
}

int Sim7020cTls::read(uint8_t* buf, size_t len)
{
    if (!m_connected) return -1;
    int r = mbedtls_ssl_read(&m_ssl, buf, len);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (r < 0) { logMbedtlsErr("TLS: read", r); return -1; }
    return r;
}

// ============================================================================
// httpsPost() — HTTP/1.1 POST через открытое TLS-соединение
// ============================================================================
int Sim7020cTls::httpsPost(const char* path,
                            const char* host,
                            const char* authB64,
                            const char* json,
                            uint16_t    jsonLen)
{
    if (!m_connected || !path || !host || !json || jsonLen == 0) return -1;

    // ---- Построить HTTP-заголовок ----
    char hdr[700];
    int hdrLen;

    if (authB64 && authB64[0]) {
        hdrLen = std::snprintf(hdr, sizeof(hdr),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: Basic %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, authB64, (unsigned)jsonLen);
    } else {
        hdrLen = std::snprintf(hdr, sizeof(hdr),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, (unsigned)jsonLen);
    }

    if (hdrLen <= 0 || (size_t)hdrLen >= sizeof(hdr)) {
        DBG.error("TLS HTTP: header overflow");
        return -50;
    }

    // ---- Отправить заголовок + тело ----
    if (write(reinterpret_cast<const uint8_t*>(hdr), (size_t)hdrLen) < 0)   return -51;
    if (write(reinterpret_cast<const uint8_t*>(json), (size_t)jsonLen) < 0) return -52;

    // ---- Читать ответ и извлечь HTTP-код ----
    static char rx[1024];
    int used = 0;
    int httpCode = -1;
    const uint32_t t0 = HAL_GetTick();

    while ((HAL_GetTick() - t0) < m_timeoutMs) {
        int r = mbedtls_ssl_read(&m_ssl,
                                  reinterpret_cast<unsigned char*>(rx + used),
                                  (size_t)(sizeof(rx) - 1 - used));

        if (r > 0) {
            used += r;
            rx[used] = '\0';

            const char* p = std::strstr(rx, "HTTP/1.");
            if (p) {
                int code = 0;
                if (std::sscanf(p, "HTTP/%*s %d", &code) == 1) {
                    httpCode = code;
                    DBG.info("TLS HTTP: code=%d", httpCode);
                    break;
                }
            }
            continue;
        }

        if (r == 0) break;  // Соединение закрыто сервером

        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            HAL_Delay(5);
            IWDG->KR = 0xAAAA;
            continue;
        }

        logMbedtlsErr("TLS HTTP: read response", r);
        break;
    }

    return httpCode;
}

// ============================================================================
// postJson() — полный цикл connect + post + close
// ============================================================================
int Sim7020cTls::postJson(const char* url,
                           const char* authB64,
                           const char* json,
                           uint16_t    jsonLen)
{
    UrlParts u{};
    if (!parseHttpsUrl(url, u)) {
        DBG.error("TLS: не удалось разобрать URL: %s", url ? url : "(null)");
        return -10;
    }

    int rc = connect(u.host, u.port);
    if (rc != 0) {
        DBG.error("TLS: connect failed rc=%d", rc);
        return -11;
    }

    int code = httpsPost(u.path, u.host, authB64, json, jsonLen);
    close();

    return code;
}

// ============================================================================
// close()
// ============================================================================
void Sim7020cTls::close()
{
    if (m_connected) {
        // TLS close_notify
        mbedtls_ssl_close_notify(&m_ssl);
        m_connected = false;
    }

    // Закрыть TCP-сокет через модем
    char cmd[32], r[64];
    std::snprintf(cmd, sizeof(cmd), "AT+CSOCL=%hhu\r\n", m_sockId);
    m_modem.sendRaw_pub(cmd, (uint16_t)std::strlen(cmd));
    m_modem.waitFor_pub(r, sizeof(r), "OK", 2000);

    // Освободить mbedTLS
    tlsFree();
}
