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

// Глобальный флаг монопольного веб-режима
volatile bool g_web_exclusive = false;

#define IWDG_FEED()  do { IWDG->KR = 0xAAAAu; } while(0)

static const char* s_caPem = nullptr;
void HttpsW5500::setCaPem(const char* caPem) { s_caPem = caPem; }

// ================================================================
// FIX: TLS контексты объявлены STATIC — они в BSS секции (RAM),
// а НЕ на стеке. Стек STM32F4 по умолчанию = 1024 bytes.
// mbedtls_entropy_context один занимает ~1636 bytes → Hard Fault!
//
// static:
//   ssl     ~304 bytes  → .bss
//   conf    ~244 bytes  → .bss
//   entropy ~1636 bytes → .bss  ← без static: STACK OVERFLOW
//   ctr     ~560 bytes  → .bss
//   cacert  ~136 bytes  → .bss
//   ИТОГО ~2880 bytes в .bss, стек не трогаем
// ================================================================
static mbedtls_ssl_context     s_ssl;
static mbedtls_ssl_config      s_conf;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_ctr;
static mbedtls_x509_crt        s_cacert;
static bool                    s_drbg_seeded = false;

// ================================================================
// Request/response буферы — тоже static
// ================================================================
static char s_hdr[600];
static char s_rx[768];

static void dumpSockState(uint8_t sn, const char* tag) {
    DBG.info("%s: Sn_SR=0x%02X Sn_IR=0x%02X", tag, getSn_SR(sn), getSn_IR(sn));
}
static void logMbedtlsErr(const char* tag, int rc) {
    char buf[128]{};
    mbedtls_strerror(rc, buf, sizeof(buf));
    DBG.error("%s rc=-0x%04X (%s)", tag, (unsigned)(-rc), buf[0] ? buf : "?");
}
static bool isIpv4Literal(const char* s) {
    if (!s || !*s) return false;
    for (const char* p = s; *p; ++p)
        if (!std::isdigit((unsigned char)*p) && *p != '.') return false;
    return true;
}
static bool resolveHost(const char* host, uint8_t outIp[4]) {
    if (isIpv4Literal(host)) {
        uint32_t a=0,b=0,c=0,d=0;
        if (std::sscanf(host, "%lu.%lu.%lu.%lu",&a,&b,&c,&d) != 4) return false;
        outIp[0]=(uint8_t)a; outIp[1]=(uint8_t)b;
        outIp[2]=(uint8_t)c; outIp[3]=(uint8_t)d;
        return true;
    }
    static uint8_t dnsBuf[512];
    DNS_init(1, dnsBuf);
    wiz_NetInfo ni{};
    wizchip_getnetinfo(&ni);
    DBG.info("DNS: server %u.%u.%u.%u", ni.dns[0],ni.dns[1],ni.dns[2],ni.dns[3]);
    uint8_t ip[4]{};
    int8_t r = DNS_run(ni.dns, (uint8_t*)host, ip);
    DBG.info("DNS: run host=%s r=%d", host, (int)r);
    if (r != 1) return false;
    std::memcpy(outIp, ip, 4);
    DBG.info("DNS: resolved %u.%u.%u.%u", ip[0],ip[1],ip[2],ip[3]);
    return true;
}

struct TlsSockCtx { uint8_t sn; uint32_t deadline; };

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

// ================================================================
// initTlsDrbg — вызывается ОДИН РАЗ за время жизни устройства.
// Seed энтропии — дорогая операция (раньше занимала 10-30с).
// После первого вызова s_drbg_seeded=true, повторно не выполняется.
// ================================================================
static bool initTlsDrbg() {
    if (s_drbg_seeded) return true;

    IWDG_FEED();
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_ctr);

    IWDG_FEED();
    DBG.info("TLS: seeding RNG...");
    const char* pers = "w5500_https";
    int rc = mbedtls_ctr_drbg_seed(&s_ctr, mbedtls_entropy_func, &s_entropy,
                                    (const unsigned char*)pers, std::strlen(pers));
    IWDG_FEED();

    if (rc != 0) {
        logMbedtlsErr("TLS: ctr_drbg_seed", rc);
        return false;
    }
    s_drbg_seeded = true;
    DBG.info("TLS: RNG seeded OK (once)");
    return true;
}

// ================================================================
// postJson — отправка JSON через HTTPS/TLS на W5500.
//
// Все TLS структуры static (в .bss, не на стеке).
// DRBG seed выполняется один раз.
// IWDG кормится на каждом шаге и в каждой итерации циклов.
// g_web_exclusive — немедленный выход если веб-клиент подключился.
// ================================================================
int HttpsW5500::postJson(const char* httpsUrl, const char* authBasicB64,
                         const char* json, uint16_t jsonLen, uint32_t timeoutMs)
{
    if (g_web_exclusive) {
        DBG.warn("HTTPS: skipped (web exclusive mode)");
        return -99;
    }

    // --- Parse URL ---
    UrlParts u{};
    if (!parseHttpsUrl(httpsUrl, u)) {
        DBG.error("HTTPS: bad URL (%s)", httpsUrl ? httpsUrl : "null");
        return -10;
    }

    // --- DNS ---
    uint8_t ip[4]{};
    if (!resolveHost(u.host, ip)) {
        DBG.error("HTTPS: DNS fail (%s)", u.host);
        return -11;
    }
    IWDG_FEED();

    // --- TCP socket ---
    DBG.info("HTTPS: connect %s:%u", u.host, (unsigned)u.port);
    DBG.info("HTTPS: ip %u.%u.%u.%u", ip[0],ip[1],ip[2],ip[3]);
    const uint8_t sn = 0;
    if (socket(sn, Sn_MR_TCP, 50001, 0) != sn) {
        DBG.error("HTTPS: socket() fail"); close(sn); return -20;
    }
    dumpSockState(sn, "HTTPS: after socket");
    if (connect(sn, ip, u.port) != SOCK_OK) {
        DBG.error("HTTPS: connect() fail");
        dumpSockState(sn, "HTTPS: connect fail");
        close(sn); return -21;
    }
    dumpSockState(sn, "HTTPS: connected");
    IWDG_FEED();

    if (g_web_exclusive) {
        DBG.warn("HTTPS: aborted after TCP connect");
        disconnect(sn); close(sn); return -99;
    }

    // --- TLS DRBG init (только 1 раз за жизнь устройства) ---
    if (!initTlsDrbg()) {
        disconnect(sn); close(sn); return -30;
    }
    IWDG_FEED();

    // --- TLS context setup (каждый раз) ---
    // Используем s_ssl, s_conf, s_cacert — static, не на стеке!
    mbedtls_ssl_free(&s_ssl);         mbedtls_ssl_init(&s_ssl);
    mbedtls_ssl_config_free(&s_conf); mbedtls_ssl_config_init(&s_conf);
    mbedtls_x509_crt_free(&s_cacert); mbedtls_x509_crt_init(&s_cacert);
    IWDG_FEED();

    if (s_caPem) {
        IWDG_FEED();
        int rc = mbedtls_x509_crt_parse(&s_cacert,
                                         (const unsigned char*)s_caPem,
                                         std::strlen(s_caPem) + 1);
        IWDG_FEED();
        if (rc < 0) { logMbedtlsErr("TLS: x509_crt_parse", rc); disconnect(sn);close(sn);return -31; }
    }

    IWDG_FEED();
    int rc = mbedtls_ssl_config_defaults(&s_conf,
                                          MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    IWDG_FEED();
    if (rc != 0) { logMbedtlsErr("TLS: config_defaults", rc); disconnect(sn);close(sn);return -32; }

    mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_ctr);
    if (s_caPem) {
        mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&s_conf, &s_cacert, nullptr);
    } else {
        mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    IWDG_FEED();
    rc = mbedtls_ssl_setup(&s_ssl, &s_conf);
    IWDG_FEED();
    if (rc != 0) { logMbedtlsErr("TLS: ssl_setup", rc); disconnect(sn);close(sn);return -33; }

    (void)mbedtls_ssl_set_hostname(&s_ssl, u.host);

    // Используем static ctx для BIO — не на стеке!
    static TlsSockCtx bio;
    bio.sn       = sn;
    bio.deadline = HAL_GetTick() + timeoutMs;
    mbedtls_ssl_set_bio(&s_ssl, &bio, w5500_send_cb, w5500_recv_cb, nullptr);

    // --- TLS handshake ---
    DBG.info("TLS: handshake...");
    while (true) {
        if (g_web_exclusive) {
            DBG.warn("HTTPS: handshake aborted (web)");
            rc = MBEDTLS_ERR_SSL_TIMEOUT; break;
        }
        rc = mbedtls_ssl_handshake(&s_ssl);
        if (rc == 0) { DBG.info("TLS: handshake OK"); break; }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (HAL_GetTick() > bio.deadline) {
                DBG.warn("TLS: handshake timeout");
                rc = MBEDTLS_ERR_SSL_TIMEOUT; break;
            }
            IWDG_FEED();
            HAL_Delay(1);
            continue;
        }
        logMbedtlsErr("TLS: handshake error", rc);
        break;
    }
    if (rc != 0) { disconnect(sn); close(sn); return -40; }

    // --- HTTP request ---
    int hdrLen = std::snprintf(s_hdr, sizeof(s_hdr),
        "POST %s HTTP/1.1\r\nHost: %s\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        u.path, u.host,
        authBasicB64 ? authBasicB64 : "",
        (unsigned)jsonLen);
    if (hdrLen <= 0 || (size_t)hdrLen >= sizeof(s_hdr)) {
        DBG.error("HTTPS: header overflow"); return -50;
    }

    auto sslWriteAll = [&](const uint8_t* p, uint32_t n) -> int {
        uint32_t off = 0;
        while (off < n) {
            if (g_web_exclusive) { DBG.warn("HTTPS: write abort (web)"); return -1; }
            int w = mbedtls_ssl_write(&s_ssl, p + off, (size_t)(n - off));
            if (w > 0) { off += (uint32_t)w; continue; }
            if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) {
                IWDG_FEED(); HAL_Delay(1); continue;
            }
            logMbedtlsErr("TLS: write error", w); return -1;
        }
        return 0;
    };
    if (sslWriteAll((const uint8_t*)s_hdr, (uint32_t)hdrLen)  != 0) return -51;
    if (sslWriteAll((const uint8_t*)json,  (uint32_t)jsonLen) != 0) return -52;

    // --- Response read ---
    int used = 0; int httpCode = -1;
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeoutMs) {
        if (g_web_exclusive) { DBG.warn("HTTPS: rx abort (web)"); break; }
        int r = mbedtls_ssl_read(&s_ssl,
                                  (unsigned char*)s_rx + used,
                                  sizeof(s_rx) - 1 - used);
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
        logMbedtlsErr("TLS: read error", r); break;
    }
    IWDG_FEED();

    (void)mbedtls_ssl_close_notify(&s_ssl);
    disconnect(sn); close(sn);

    if (httpCode > 0) DBG.info("HTTPS: done, code=%d", httpCode);
    else              DBG.warn("HTTPS: no HTTP code (rx=%d bytes)", used);
    return httpCode;
}
