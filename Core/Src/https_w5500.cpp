#include "https_w5500.hpp"
#include "debug_uart.hpp"
#include <cstring>
#include <cstdio>
#include <cctype>

extern "C" {
#include "socket.h"
#include "dns.h"
#include "w5500.h"
#include "wizchip_conf.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "stm32f4xx_hal.h"
}

volatile bool g_web_exclusive = false;

#define IWDG_FEED()  do { IWDG->KR = 0xAAAAu; } while(0)

// ================================================================
// ДИАГНОСТИЧЕСКИЙ МАКРОС
// Печатает шаг и ждёт завершения UART TX, чтобы сообщение дошло
// ДАЖЕ если следующая строка вызовет Hard Fault.
// ================================================================
#define TLS_STEP(n, msg)  do { \
    DBG.info("TLS_STEP %d: " msg, (int)(n)); \
    HAL_Delay(5); \
    IWDG_FEED(); \
} while(0)

static const char* s_caPem = nullptr;
void HttpsW5500::setCaPem(const char* caPem) { s_caPem = caPem; }

// ================================================================
// ВСЕ структуры static — в .bss, не на стеке.
// Флаги init_done — не вызываем free на нулевой struct (может краш).
// ================================================================
static mbedtls_ssl_context       s_ssl;
static mbedtls_ssl_config        s_conf;
static mbedtls_entropy_context   s_entropy;
static mbedtls_ctr_drbg_context  s_ctr;
static mbedtls_x509_crt          s_cacert;

static bool s_drbg_seeded   = false;
static bool s_ssl_init_done = false;   // FIX: не вызываем free до первого init

// Буферы запроса/ответа — тоже static
static char s_hdr[512];
static char s_rx[512];


struct TlsSockCtx { uint8_t sn; uint32_t deadline; };
static TlsSockCtx s_bio;

static void logMbedtlsErr(const char* tag, int rc) {
    char buf[128]{};
    mbedtls_strerror(rc, buf, sizeof(buf));
    DBG.error("%s rc=-0x%04X (%s)", tag, (unsigned)(-rc), buf[0] ? buf : "?");
}

static bool resolveHost(const char* host, uint8_t outIp[4]) {
    // try as IP literal first
    bool isNum = true;
    for (const char* p = host; *p; ++p)
        if (!std::isdigit((unsigned char)*p) && *p != '.') { isNum = false; break; }
    if (isNum) {
        uint32_t a=0,b=0,c=0,d=0;
        if (std::sscanf(host, "%lu.%lu.%lu.%lu",&a,&b,&c,&d) != 4) return false;
        outIp[0]=(uint8_t)a; outIp[1]=(uint8_t)b;
        outIp[2]=(uint8_t)c; outIp[3]=(uint8_t)d;
        return true;
    }
    static uint8_t dnsBuf[512];
    DNS_init(1, dnsBuf);
    wiz_NetInfo ni{}; wizchip_getnetinfo(&ni);
    DBG.info("DNS: server %u.%u.%u.%u", ni.dns[0],ni.dns[1],ni.dns[2],ni.dns[3]);
    uint8_t ip[4]{};
    int8_t r = DNS_run(ni.dns, (uint8_t*)host, ip);
    DBG.info("DNS: run host=%s r=%d", host, (int)r);
    if (r != 1) return false;
    std::memcpy(outIp, ip, 4);
    DBG.info("DNS: resolved %u.%u.%u.%u", ip[0],ip[1],ip[2],ip[3]);
    return true;
}

static int w5500_send_cb(void* ctx, const unsigned char* buf, size_t len) {
    auto* c = (TlsSockCtx*)ctx;
    if (len == 0) return 0;
    uint16_t fsr = getSn_TX_FSR(c->sn);
    if (fsr == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    uint16_t toSend = (len > fsr) ? fsr : (uint16_t)len;
    int32_t r = send(c->sn, (uint8_t*)buf, toSend);
    if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (r == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)r;
}
static int w5500_recv_cb(void* ctx, unsigned char* buf, size_t len) {
    auto* c = (TlsSockCtx*)ctx;
    if (len == 0) return 0;
    uint16_t rsr = getSn_RX_RSR(c->sn);
    if (rsr == 0) {
        if (HAL_GetTick() > c->deadline) return MBEDTLS_ERR_SSL_TIMEOUT;
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    uint16_t toRead = (len > rsr) ? rsr : (uint16_t)len;
    int32_t r = recv(c->sn, (uint8_t*)buf, toRead);
    if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)r;
}

bool HttpsW5500::parseHttpsUrl(const char* url, UrlParts& out) {
    if (!url) return false;
    std::memset(&out, 0, sizeof(out));
    out.port = 443;
    const char* p = url;
    if (std::strncmp(p, "https://", 8) != 0) return false;
    p += 8;
    const char* hb = p;
    while (*p && *p != '/' && *p != ':') p++;
    size_t hl = (size_t)(p - hb);
    if (hl == 0 || hl >= sizeof(out.host)) return false;
    std::memcpy(out.host, hb, hl); out.host[hl] = 0;
    if (*p == ':') {
        p++; uint32_t port = 0;
        while (*p && std::isdigit((unsigned char)*p))
            { port = port * 10u + (uint32_t)(*p - '0'); p++; }
        if (port == 0 || port > 65535) return false;
        out.port = (uint16_t)port;
    }
    if (*p == 0) { std::strcpy(out.path, "/"); return true; }
    if (*p != '/') return false;
    size_t pl = std::strlen(p);
    if (pl == 0 || pl >= sizeof(out.path)) return false;
    std::memcpy(out.path, p, pl + 1);
    return true;
}

int HttpsW5500::postJson(const char* httpsUrl, const char* authB64,
                         const char* json, uint16_t jsonLen, uint32_t timeoutMs)
{
    if (g_web_exclusive) { DBG.warn("HTTPS: skip (web)"); return -99; }

    // UrlParts — 194 bytes на стеке, это ОК (TLS контексты уже в .bss)
    UrlParts u{};

    TLS_STEP(1, "parse URL");
    if (!parseHttpsUrl(httpsUrl, u)) {
        DBG.error("HTTPS: bad URL"); return -10;
    }

    TLS_STEP(2, "DNS resolve");
    static uint8_t s_ip[4];
    if (!resolveHost(u.host, s_ip)) {
        DBG.error("HTTPS: DNS fail"); return -11;
    }
    IWDG_FEED();

    TLS_STEP(3, "TCP socket+connect");
    DBG.info("HTTPS: connect %s:%u", u.host, (unsigned)u.port);
    DBG.info("HTTPS: ip %u.%u.%u.%u", s_ip[0],s_ip[1],s_ip[2],s_ip[3]);
    const uint8_t sn = 0;
    if (socket(sn, Sn_MR_TCP, 50001, 0) != sn) {
        DBG.error("HTTPS: socket() fail"); close(sn); return -20;
    }
    DBG.info("HTTPS: after socket: Sn_SR=0x%02X Sn_IR=0x%02X",
             getSn_SR(sn), getSn_IR(sn));
    if (connect(sn, s_ip, u.port) != SOCK_OK) {
        DBG.error("HTTPS: connect() fail"); close(sn); return -21;
    }
    DBG.info("HTTPS: connected: Sn_SR=0x%02X Sn_IR=0x%02X",
             getSn_SR(sn), getSn_IR(sn));
    IWDG_FEED();

    if (g_web_exclusive) {
        DBG.warn("HTTPS: abort after connect"); disconnect(sn); close(sn); return -99;
    }

    // ================================================================
    // DRBG seed — выполняется ОДИН РАЗ за всё время работы устройства
    // ================================================================
    if (!s_drbg_seeded) {
        TLS_STEP(4, "entropy_init");
        mbedtls_entropy_init(&s_entropy);

        TLS_STEP(5, "ctr_drbg_init");
        mbedtls_ctr_drbg_init(&s_ctr);

        TLS_STEP(6, "ctr_drbg_seed START");
        IWDG_FEED();
        const char* pers = "w5500";
        int rc = mbedtls_ctr_drbg_seed(&s_ctr, mbedtls_entropy_func, &s_entropy,
                                        (const unsigned char*)pers, 5);
        IWDG_FEED();
        TLS_STEP(7, "ctr_drbg_seed DONE");
        if (rc != 0) {
            logMbedtlsErr("TLS: seed", rc);
            disconnect(sn); close(sn); return -30;
        }
        s_drbg_seeded = true;
        DBG.info("TLS: RNG seeded OK (once)");
    } else {
        DBG.info("TLS: RNG reuse (already seeded)");
        IWDG_FEED();
    }

    // ================================================================
    // SSL context setup — каждый раз заново, но static структуры
    // FIX: не вызываем mbedtls_ssl_free на нулевой static (первый вызов)
    // ================================================================
    TLS_STEP(8, "ssl context reset");
    if (s_ssl_init_done) {
        mbedtls_ssl_free(&s_ssl);
        mbedtls_ssl_config_free(&s_conf);
        mbedtls_x509_crt_free(&s_cacert);
    }
    mbedtls_ssl_init(&s_ssl);
    mbedtls_ssl_config_init(&s_conf);
    mbedtls_x509_crt_init(&s_cacert);
    s_ssl_init_done = true;
    IWDG_FEED();

    TLS_STEP(9, "ssl_config_defaults");
    int rc = mbedtls_ssl_config_defaults(&s_conf,
                                          MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    IWDG_FEED();
    if (rc != 0) {
        logMbedtlsErr("TLS: config_defaults", rc);
        disconnect(sn); close(sn); return -32;
    }

    TLS_STEP(10, "ssl_conf_rng + authmode");
    mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_ctr);
    mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_NONE);
    IWDG_FEED();

    TLS_STEP(11, "ssl_setup");
    rc = mbedtls_ssl_setup(&s_ssl, &s_conf);
    IWDG_FEED();
    if (rc != 0) {
        logMbedtlsErr("TLS: ssl_setup", rc);
        disconnect(sn); close(sn); return -33;
    }

    TLS_STEP(12, "ssl_set_hostname + set_bio");
    (void)mbedtls_ssl_set_hostname(&s_ssl, u.host);
    s_bio.sn       = sn;
    s_bio.deadline = HAL_GetTick() + timeoutMs;
    mbedtls_ssl_set_bio(&s_ssl, &s_bio, w5500_send_cb, w5500_recv_cb, nullptr);
    IWDG_FEED();

    TLS_STEP(13, "handshake loop START");
    while (true) {
        if (g_web_exclusive) {
            DBG.warn("HTTPS: handshake abort (web)"); rc = MBEDTLS_ERR_SSL_TIMEOUT; break;
        }
        rc = mbedtls_ssl_handshake(&s_ssl);
        if (rc == 0) { DBG.info("TLS: handshake OK"); break; }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (HAL_GetTick() > s_bio.deadline) {
                DBG.warn("TLS: handshake timeout"); rc = MBEDTLS_ERR_SSL_TIMEOUT; break;
            }
            IWDG_FEED();
            HAL_Delay(1);
            continue;
        }
        logMbedtlsErr("TLS: handshake error", rc);
        break;
    }
    if (rc != 0) { disconnect(sn); close(sn); return -40; }

    TLS_STEP(14, "HTTP write");
    int hdrLen = std::snprintf(s_hdr, sizeof(s_hdr),
        "POST %s HTTP/1.1\r\nHost: %s\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        u.path, u.host,
        authB64 ? authB64 : "", (unsigned)jsonLen);

    auto sslWriteAll = [&](const uint8_t* p, uint32_t n) -> int {
        uint32_t off = 0;
        while (off < n) {
            if (g_web_exclusive) return -1;
            int w = mbedtls_ssl_write(&s_ssl, p + off, (size_t)(n - off));
            if (w > 0) { off += (uint32_t)w; continue; }
            if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) {
                IWDG_FEED(); HAL_Delay(1); continue;
            }
            logMbedtlsErr("TLS: write", w); return -1;
        }
        return 0;
    };
    if (hdrLen > 0 && sslWriteAll((const uint8_t*)s_hdr,  (uint32_t)hdrLen) != 0) return -51;
    if (             sslWriteAll((const uint8_t*)json,     (uint32_t)jsonLen) != 0) return -52;

    TLS_STEP(15, "HTTP read");
    int used = 0; int httpCode = -1;
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeoutMs) {
        if (g_web_exclusive) { DBG.warn("HTTPS: rx abort (web)"); break; }
        int r = mbedtls_ssl_read(&s_ssl,
                                  (unsigned char*)s_rx + used,
                                  sizeof(s_rx) - 1 - (size_t)used);
        if (r > 0) {
            used += r; s_rx[used] = 0;
            const char* p = std::strstr(s_rx, "HTTP/1.1 ");
            if (!p) p = std::strstr(s_rx, "HTTP/1.0 ");
            if (p) {
                int code = 0;
                if (std::sscanf(p, "HTTP/%*s %d", &code) == 1 && code > 0) {
                    httpCode = code; break;
                }
            }
            if (used >= (int)(sizeof(s_rx) - 1)) break;
            continue;
        }
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            IWDG_FEED(); HAL_Delay(2); continue;
        }
        logMbedtlsErr("TLS: read", r); break;
    }
    IWDG_FEED();

    (void)mbedtls_ssl_close_notify(&s_ssl);
    disconnect(sn); close(sn);

    if (httpCode > 0) DBG.info("HTTPS: done, code=%d", httpCode);
    else              DBG.warn("HTTPS: no code (rx=%d)", used);
    return httpCode;
}
