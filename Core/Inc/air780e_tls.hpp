/**
 * ================================================================
 * @file    air780e_tls.hpp
 * @brief   TLS-туннель поверх Air780E TCP-сокета (mbedTLS BIO).
 *
 * Архитектура идентична sim7020c_tls:
 *   mbedTLS → biosend/biorecv → AT+CIPSEND/CIPRXGET → Air780E UART
 *
 * AT-команды (Air780E AT Command Manual):
 *   Открытие TCP : AT+CIPOPEN=<id>,"TCP","<host>",<port>
 *   Отправка     : AT+CIPSEND=<id>,<len>  → prompt ">" → бинарные данные
 *   Приём        : AT+CIPRXGET=2,<id>,<len>
 *   Закрытие     : AT+CIPCLOSE=<id>
 *
 * Ограничения:
 *   - CIPSEND: макс. 1460 байт за раз (MTU TCP)
 *   - CIPRXGET: макс. 1460 байт за раз
 *   - mbedTLS требует ~40–80 КБ RAM (STM32F407: 192 КБ — достаточно)
 * ================================================================
 */
#pragma once

#include "air780e.hpp"
#include "gsm_common.hpp"

extern "C" {
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
}

class Air780eTls {
public:
    /**
     * @param modem      Уже инициализированный объект Air780E
     * @param timeoutMs  Таймаут на всю TLS-операцию (мс)
     */
    explicit Air780eTls(Air780E& modem, uint32_t timeoutMs = 30000);
    ~Air780eTls() { tlsFree(); }

    /**
     * Задать CA-сертификат (PEM, NULL-terminated).
     * Если не задан — MBEDTLS_SSL_VERIFY_NONE (шифрование без проверки).
     */
    void setCaCert(const char* pem) { m_caPem = pem; }

    /**
     * Полный цикл: TCP connect + TLS handshake + HTTP POST + close.
     * @param url     "https://host[:port]/path"
     * @param authB64 Authorization Basic header value (или nullptr)
     * @param json    Тело запроса (бинарный буфер, не обязательно \0)
     * @param jsonLen Длина тела в байтах
     * @return HTTP-код ответа (200, 400 ...) или отрицательный код ошибки
     */
    int postJson(const char* url, const char* authB64,
                 const char* json, uint16_t jsonLen);

    /** Открыть TCP + TLS соединение */
    int  connect(const char* host, uint16_t port);
    /** Отправить данные через TLS */
    int  write(const uint8_t* buf, size_t len);
    /** Принять данные через TLS */
    int  read(uint8_t* buf, size_t len);
    /** Закрыть TLS + TCP */
    void close();

private:
    struct UrlParts {
        char     host[64]{};
        char     path[128]{};
        uint16_t port = 443;
    };

    Air780E&    m_modem;
    uint32_t    m_timeoutMs;
    uint32_t    m_deadline   = 0;
    uint8_t     m_linkNo     = 0;    ///< Air780E CIP link number (0..9)
    bool        m_connected  = false;
    bool        m_tlsInited  = false;
    const char* m_caPem      = nullptr;

    mbedtls_ssl_context     m_ssl{};
    mbedtls_ssl_config      m_conf{};
    mbedtls_entropy_context m_entropy{};
    mbedtls_ctr_drbg_context m_ctr{};
    mbedtls_x509_crt        m_cacert{};

    static constexpr uint16_t MODEM_MAX_SEND = 512;
    static constexpr uint16_t MODEM_MAX_RECV = 512;

    bool parseHttpsUrl(const char* url, UrlParts& out);
    int  modemWriteRaw(const uint8_t* buf, uint16_t len);
    int  modemReadRaw(uint8_t* buf, uint16_t len);
    int  httpsPost(const char* path, const char* host,
                   const char* authB64, const char* json, uint16_t jsonLen);
    void tlsFree();
    void logMbedtlsErr(const char* tag, int rc);

    // mbedTLS BIO callbacks
    static int biosend(void* ctx, const unsigned char* buf, size_t len);
    static int biorecv(void* ctx, unsigned char* buf, size_t len);
};

// Алиас для единообразия с sim7020c_tls в остальном коде
using GsmTls = Air780eTls;
