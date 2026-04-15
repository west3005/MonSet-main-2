/**
 * ================================================================
 * @file    air780e_tls.cpp
 * @brief   TLS поверх Air780E (mbedTLS + AT+CIP* TCP).
 * ================================================================
 */
#include "air780e_tls.hpp"
#include "debug_uart.hpp"
#include "board_pins.hpp"

extern "C" {
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

// ============================================================================
// Конструктор
// ============================================================================
Air780eTls::Air780eTls(Air780E& modem, uint32_t timeoutMs)
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
void Air780eTls::logMbedtlsErr(const char* tag, int rc)
{
    char buf[128];
    std::memset(buf, 0, sizeof(buf));
    mbedtls_strerror(rc, buf, sizeof(buf));
    DBG.error("%s rc=-0x%04X (%s)", tag, (unsigned)(-rc), buf[0] ? buf : "?");
}

void Air780eTls::tlsFree()
{
    if (!m_tlsInited) return;
    mbedtls_ssl_free(&m_ssl);
    mbedtls_ssl_config_free(&m_conf);
    mbedtls_x509_crt_free(&m_cacert);
    mbedtls_ctr_drbg_free(&m_ctr);
    mbedtls_entropy_free(&m_entropy);
    // Реинициализируем для повторного использования
    mbedtls_ssl_init(&m_ssl);
    mbedtls_ssl_config_init(&m_conf);
    mbedtls_entropy_init(&m_entropy);
    mbedtls_ctr_drbg_init(&m_ctr);
    mbedtls_x509_crt_init(&m_cacert);
}

bool Air780eTls::parseHttpsUrl(const char* url, UrlParts& out)
{
    if (!url) return false;
    out = UrlParts{};
    out.port = 443;

    const char* prefix = "https://";
    size_t prefixLen = std::strlen(prefix);
    if (std::strncmp(url, prefix, prefixLen) != 0) return false;

    const char* p = url + prefixLen;
    const char* hb = p;
    while (*p && *p != '/' && *p != ':') p++;
    size_t hl = (size_t)(p - hb);
    if (hl == 0 || hl >= sizeof(out.host)) return false;
    std::memcpy(out.host, hb, hl);
    out.host[hl] = '\0';

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
    size_t pl = std::strlen(p);
    if (pl == 0 || pl >= sizeof(out.path)) return false;
    std::memcpy(out.path, p, pl + 1);
    return true;
}

// ============================================================================
// BIO: отправка через AT+CIPSEND=<linkNo>,<len>
// ============================================================================
int Air780eTls::modemWriteRaw(const uint8_t* buf, uint16_t len)
{
    if (!buf || len == 0) return 0;

    uint16_t sent = 0;
    char cmd[64], r[128];

    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > MODEM_MAX_SEND) chunk = MODEM_MAX_SEND;

        // AT+CIPSEND=<linkNo>,<len>
        std::snprintf(cmd, sizeof(cmd),
                      "AT+CIPSEND=%hhu,%u\r\n", m_linkNo, (unsigned)chunk);
        m_modem.sendRaw_pub(cmd, (uint16_t)std::strlen(cmd));

        // Ждём prompt ">"
        m_modem.waitFor_pub(r, sizeof(r), ">", 3000);
        if (!std::strstr(r, ">")) {
            DBG.error("TLS BIO: нет prompt '>' для CIPSEND");
            return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        }

        // Отправляем бинарные данные
        m_modem.sendRaw_pub(reinterpret_cast<const char*>(buf + sent), chunk);

        // Ждём SEND OK
        m_modem.waitFor_pub(r, sizeof(r), "SEND OK", 5000);
        if (!std::strstr(r, "SEND OK") && !std::strstr(r, "OK")) {
            DBG.warn("TLS BIO: CIPSEND не ответил SEND OK (chunk=%u)", (unsigned)chunk);
        }

        sent += chunk;
        IWDG->KR = 0xAAAA;
    }
    return (int)sent;
}

// ============================================================================
// BIO: приём через AT+CIPRXGET=2,<linkNo>,<len>
// ============================================================================
int Air780eTls::modemReadRaw(uint8_t* buf, uint16_t len)
{
    if (!buf || len == 0) return 0;

    uint16_t want = (len > MODEM_MAX_RECV) ? MODEM_MAX_RECV : len;
    char cmd[64];
    char r[MODEM_MAX_RECV + 128];

    // Включить режим ручного чтения (если ещё не включён)
    // AT+CIPRXGET=1 — активировать буферизацию (делается один раз в connect)

    // AT+CIPRXGET=2,<linkNo>,<len>
    std::snprintf(cmd, sizeof(cmd),
                  "AT+CIPRXGET=2,%hhu,%u\r\n", m_linkNo, (unsigned)want);
    m_modem.sendRaw_pub(cmd, (uint16_t)std::strlen(cmd));

    // Ответ формата: +CIPRXGET: 2,<linkNo>,<actual_len>,<pending_len>\r\n<data>\r\nOK
    uint16_t rxLen = m_modem.waitFor_pub(r, sizeof(r), "+CIPRXGET:", 3000);
    (void)rxLen;

    if (!std::strstr(r, "+CIPRXGET:")) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    // Парсим: +CIPRXGET: 2,<id>,<actual>,<pending>
    const char* p = std::strstr(r, "+CIPRXGET:");
    int mode = 0, id = 0, actual = 0, pending = 0;
    if (std::sscanf(p, "+CIPRXGET: %d,%d,%d,%d",
                    &mode, &id, &actual, &pending) < 3 || actual <= 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    // Данные идут после "\r\n" следующего за заголовком
    const char* dataStart = std::strchr(p, '\n');
    if (!dataStart) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    dataStart++;

    uint16_t copy = (uint16_t)actual;
    if (copy > len) copy = (uint16_t)len;
    size_t available = rxLen - (size_t)(dataStart - r);
    if (available < copy) copy = (uint16_t)available;

    std::memcpy(buf, dataStart, copy);
    IWDG->KR = 0xAAAA;
    return (int)copy;
}

// ============================================================================
// mbedTLS BIO callbacks
// ============================================================================
int Air780eTls::biosend(void* ctx, const unsigned char* buf, size_t len)
{
    auto* self = static_cast<Air780eTls*>(ctx);
    if (!self || !buf || len == 0) return 0;
    if (HAL_GetTick() > self->m_deadline) return MBEDTLS_ERR_SSL_TIMEOUT;

    uint16_t toSend = (len > MODEM_MAX_SEND) ? MODEM_MAX_SEND : (uint16_t)len;
    int r = self->modemWriteRaw(buf, toSend);
    if (r <= 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return r;
}

int Air780eTls::biorecv(void* ctx, unsigned char* buf, size_t len)
{
    auto* self = static_cast<Air780eTls*>(ctx);
    if (!self || !buf || len == 0) return 0;
    if (HAL_GetTick() > self->m_deadline) return MBEDTLS_ERR_SSL_TIMEOUT;

    uint16_t toRead = (len > MODEM_MAX_RECV) ? MODEM_MAX_RECV : (uint16_t)len;
    int r = self->modemReadRaw(buf, toRead);
    if (r == MBEDTLS_ERR_SSL_WANT_READ) return r;
    if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (r == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    return r;
}

// ============================================================================
// connect(): TCP open + включение CIPRXGET + TLS handshake
// ============================================================================
int Air780eTls::connect(const char* host, uint16_t port)
{
    DBG.info("TLS: connect %s:%u", host, port);
    m_deadline = HAL_GetTick() + m_timeoutMs;

    char r[256], cmd[128];

    // Закрыть предыдущий линк если был
    std::snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%hhu\r\n", m_linkNo);
    m_modem.sendRaw_pub(cmd, (uint16_t)std::strlen(cmd));
    HAL_Delay(200);

    // Включить режим ручного буферизованного чтения
    // AT+CIPRXGET=1 — данные НЕ поступают автоматически, читаем через CIPRXGET=2
    m_modem.sendRaw_pub("AT+CIPRXGET=1\r\n", 15);
    m_modem.waitFor_pub(r, sizeof(r), "OK", 2000);

    // AT+CIPOPEN=<linkNo>,"TCP","<host>",<port>
    std::snprintf(cmd, sizeof(cmd),
                  "AT+CIPOPEN=%hhu,\"TCP\",\"%s\",%u\r\n",
                  m_linkNo, host, port);
    m_modem.sendRaw_pub(cmd, (uint16_t)std::strlen(cmd));

    // Ответ: +CIPOPEN: <linkNo>,0  (0 = success)
    m_modem.waitFor_pub(r, sizeof(r), "+CIPOPEN:", 15000);
    if (!std::strstr(r, "+CIPOPEN:")) {
        DBG.error("TLS: CIPOPEN нет ответа host=%s:%u", host, port);
        return -1;
    }
    {
        int id = 0, err = -1;
        const char* p = std::strstr(r, "+CIPOPEN:");
        std::sscanf(p, "+CIPOPEN: %d,%d", &id, &err);
        if (err != 0) {
            DBG.error("TLS: CIPOPEN err=%d host=%s:%u", err, host, port);
            return -2;
        }
    }
    DBG.info("TLS: TCP open OK (link=%hhu)", m_linkNo);

    // ---- mbedTLS seed ----
    const char* pers = "air780e_tls";
    int rc = mbedtls_ctr_drbg_seed(&m_ctr,
                                    mbedtls_entropy_func, &m_entropy,
                                    reinterpret_cast<const unsigned char*>(pers),
                                    std::strlen(pers));
    if (rc != 0) { logMbedtlsErr("TLS: ctr_drbg_seed", rc); close(); return -10; }

    // ---- CA cert ----
    if (m_caPem) {
        rc = mbedtls_x509_crt_parse(&m_cacert,
                                     reinterpret_cast<const unsigned char*>(m_caPem),
                                     std::strlen(m_caPem) + 1);
        if (rc < 0) { logMbedtlsErr("TLS: x509_crt_parse", rc); close(); return -11; }
    }

    // ---- SSL config ----
    rc = mbedtls_ssl_config_defaults(&m_conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { logMbedtlsErr("TLS: config_defaults", rc); close(); return -12; }

    mbedtls_ssl_conf_rng(&m_conf, mbedtls_ctr_drbg_random, &m_ctr);

    if (m_caPem) {
        mbedtls_ssl_conf_authmode(&m_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&m_conf, &m_cacert, nullptr);
    } else {
        mbedtls_ssl_conf_authmode(&m_conf, MBEDTLS_SSL_VERIFY_NONE);
        DBG.warn("TLS: VERIFY_NONE (CA cert не задан)");
    }

    // ---- SSL setup ----
    rc = mbedtls_ssl_setup(&m_ssl, &m_conf);
    if (rc != 0) { logMbedtlsErr("TLS: ssl_setup", rc); close(); return -13; }

    rc = mbedtls_ssl_set_hostname(&m_ssl, host);
    if (rc != 0) { logMbedtlsErr("TLS: set_hostname", rc); close(); return -14; }

    mbedtls_ssl_set_bio(&m_ssl, this,
                         Air780eTls::biosend,
                         Air780eTls::biorecv,
                         nullptr);

    // ---- TLS handshake ----
    DBG.info("TLS: handshake start...");
    while (true) {
        rc = mbedtls_ssl_handshake(&m_ssl);
        if (rc == 0) break;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
            rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (HAL_GetTick() > m_deadline) {
                DBG.error("TLS: handshake timeout");
                close(); return -20;
            }
            HAL_Delay(5);
            IWDG->KR = 0xAAAA;
            continue;
        }
        logMbedtlsErr("TLS: handshake", rc);
        close(); return -21;
    }

    DBG.info("TLS: OK, cipher=%s", mbedtls_ssl_get_ciphersuite(&m_ssl));
    m_connected = true;
    return 0;
}

// ============================================================================
// write / read
// ============================================================================
int Air780eTls::write(const uint8_t* buf, size_t len)
{
    if (!m_connected) return -1;
    size_t off = 0;
    while (off < len) {
        int w = mbedtls_ssl_write(&m_ssl, buf + off, len - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w == MBEDTLS_ERR_SSL_WANT_READ ||
            w == MBEDTLS_ERR_SSL_WANT_WRITE) {
            HAL_Delay(2); IWDG->KR = 0xAAAA; continue;
        }
        logMbedtlsErr("TLS: write", w);
        return -1;
    }
    return (int)off;
}

int Air780eTls::read(uint8_t* buf, size_t len)
{
    if (!m_connected) return -1;
    int r = mbedtls_ssl_read(&m_ssl, buf, len);
    if (r == MBEDTLS_ERR_SSL_WANT_READ ||
        r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (r < 0) { logMbedtlsErr("TLS: read", r); return -1; }
    return r;
}

// ============================================================================
// httpsPost
// ============================================================================
int Air780eTls::httpsPost(const char* path, const char* host,
                           const char* authB64,
                           const char* json, uint16_t jsonLen)
{
    if (!m_connected || !path || !host || !json || jsonLen == 0) return -1;

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

    if (write(reinterpret_cast<const uint8_t*>(hdr), (size_t)hdrLen) < 0)
        return -51;
    if (write(reinterpret_cast<const uint8_t*>(json), (size_t)jsonLen) < 0)
        return -52;

    // Читаем ответ и ищем HTTP-код
    static char rx[1024];
    int used = 0, httpCode = -1;
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
        if (r == 0) break;
        if (r == MBEDTLS_ERR_SSL_WANT_READ ||
            r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            HAL_Delay(5); IWDG->KR = 0xAAAA; continue;
        }
        logMbedtlsErr("TLS HTTP: read response", r);
        break;
    }
    return httpCode;
}

// ============================================================================
// postJson — полный цикл
// ============================================================================
int Air780eTls::postJson(const char* url, const char* authB64,
                          const char* json, uint16_t jsonLen)
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
// close
// ============================================================================
void Air780eTls::close()
{
    if (m_connected) {
        mbedtls_ssl_close_notify(&m_ssl);
        m_connected = false;
    }

    // Закрыть TCP линк
    char cmd[32], r[64];
    std::snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%hhu\r\n", m_linkNo);
    m_modem.sendRaw_pub(cmd, (uint16_t)std::strlen(cmd));
    m_modem.waitFor_pub(r, sizeof(r), "OK", 2000);

    tlsFree();
}
