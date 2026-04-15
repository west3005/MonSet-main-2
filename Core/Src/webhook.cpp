/**
 * ================================================================
 * @file    webhook.cpp
 * @brief   HTTP webhook with template substitution.
 *
 * @note    Reuses httpPostPlainW5500 pattern from app.cpp.
 *          Estimated Flash: ~2 KB, RAM: ~512 bytes (payload buffer).
 * ================================================================
 */
#include "webhook.hpp"
#include "https_w5500.hpp"
#include "debug_uart.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>

extern "C" {
#include "socket.h"
#include "dns.h"
#include "wizchip_conf.h"
}

// ================================================================
// Constructor
// ================================================================
Webhook::Webhook() {}

// ================================================================
// Init
// ================================================================
void Webhook::init(Air780E* gsm) {
    m_gsm = gsm;
    m_lastScheduledTick = HAL_GetTick();
    DBG.info("Webhook: init");
}

// ================================================================
// Check configured
// ================================================================
bool Webhook::isConfigured() const {
    return Cfg().webhook_url[0] != 0;
}

// ================================================================
// Scheduled check
// ================================================================
bool Webhook::isScheduledDue() const {
    if (Cfg().webhook_trigger != WebhookTrigger::Schedule) return false;
    uint32_t interval = Cfg().webhook_interval_sec * 1000UL;
    return (HAL_GetTick() - m_lastScheduledTick) >= interval;
}

void Webhook::markScheduledSent() {
    m_lastScheduledTick = HAL_GetTick();
}

// ================================================================
// Template substitution
// ================================================================
int Webhook::substituteTemplate(const char* tmpl, char* out, size_t outSz,
                                 float value, const char* ts, const char* name) {
    if (!tmpl || !out || outSz == 0) return -1;

    size_t pos = 0;
    const char* p = tmpl;

    while (*p && (pos + 1) < outSz) {
        // Check for {{...}} placeholder
        if (p[0] == '{' && p[1] == '{') {
            const char* end = std::strstr(p + 2, "}}");
            if (end) {
                size_t keyLen = (size_t)(end - p - 2);
                char key[32]{};
                if (keyLen < sizeof(key)) {
                    std::memcpy(key, p + 2, keyLen);
                    key[keyLen] = 0;

                    char replacement[64]{};
                    if (std::strcmp(key, "value") == 0) {
                        std::snprintf(replacement, sizeof(replacement), "%.3f", (double)value);
                    } else if (std::strcmp(key, "timestamp") == 0) {
                        if (ts) std::strncpy(replacement, ts, sizeof(replacement) - 1);
                    } else if (std::strcmp(key, "sensor_name") == 0) {
                        if (name) std::strncpy(replacement, name, sizeof(replacement) - 1);
                    } else {
                        // Unknown placeholder — keep as-is
                        replacement[0] = 0;
                    }

                    size_t rLen = std::strlen(replacement);
                    if (pos + rLen < outSz) {
                        std::memcpy(out + pos, replacement, rLen);
                        pos += rLen;
                    }
                    p = end + 2; // skip }}
                    continue;
                }
            }
        }

        out[pos++] = *p++;
    }
    out[pos] = 0;
    return (int)pos;
}

// ================================================================
// DNS + HTTP POST via W5500 (plain HTTP only — HTTPS uses HttpsW5500)
// ================================================================
bool Webhook::postViaW5500(const char* url, const char* payload, uint16_t len) {
    // Determine if HTTPS or HTTP
    if (std::strncmp(url, "https://", 8) == 0) {
        int code = HttpsW5500::postJson(url, nullptr, payload, len, 20000);
        return (code >= 200 && code < 300);
    }

    // Plain HTTP POST (reuse pattern from app.cpp)
    if (std::strncmp(url, "http://", 7) != 0) {
        DBG.error("Webhook: unsupported URL scheme");
        return false;
    }

    // Parse URL
    const char* p = url + 7;
    char host[64]{};
    char path[128] = "/";
    uint16_t port = 80;

    {
        const char* hb = p;
        while (*p && *p != '/' && *p != ':') p++;
        size_t hl = (size_t)(p - hb);
        if (hl == 0 || hl >= sizeof(host)) return false;
        std::memcpy(host, hb, hl);
        if (*p == ':') {
            p++;
            port = (uint16_t)std::strtoul(p, nullptr, 10);
            while (*p && *p != '/') p++;
        }
        if (*p == '/') std::strncpy(path, p, sizeof(path) - 1);
    }

    // DNS resolve
    uint8_t dstIp[4]{};
    {
        bool isNum = true;
        for (const char* pp = host; *pp; pp++) {
            if (!std::isdigit((unsigned char)*pp) && *pp != '.') { isNum = false; break; }
        }
        if (isNum) {
            unsigned a=0,b=0,c=0,d=0;
            std::sscanf(host, "%u.%u.%u.%u", &a,&b,&c,&d);
            dstIp[0]=(uint8_t)a; dstIp[1]=(uint8_t)b;
            dstIp[2]=(uint8_t)c; dstIp[3]=(uint8_t)d;
        } else {
            wiz_NetInfo ni{};
            wizchip_getnetinfo(&ni);
            static uint8_t dnsBuf[512];
            DNS_init(1, dnsBuf);
            bool ok = false;
            uint32_t t0 = HAL_GetTick();
            while ((HAL_GetTick() - t0) < 5000) {
                int8_t r = DNS_run(ni.dns, (uint8_t*)host, dstIp);
                if (r == 1) { ok = true; break; }
                if (r < 0) break;
                HAL_Delay(50);
                IWDG->KR = 0xAAAA;
            }
            if (!ok) { DBG.error("Webhook: DNS fail"); return false; }
        }
    }

    // TCP connect
    const uint8_t sn = 4; // Use socket 4 for webhook
    if (socket(sn, Sn_MR_TCP, 50004, 0) != sn) { close(sn); return false; }
    if (connect(sn, dstIp, port) != SOCK_OK)    { close(sn); return false; }

    // Build HTTP header
    const RuntimeConfig& c = Cfg();
    char hdr[512];
    int hdrLen = std::snprintf(hdr, sizeof(hdr),
        "%s %s HTTP/1.1\r\nHost: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n",
        c.webhook_method, path, host, (unsigned)len);

    // Add custom headers
    if (c.webhook_headers[0]) {
        hdrLen += std::snprintf(hdr + hdrLen, sizeof(hdr) - hdrLen,
                                "%s\r\n", c.webhook_headers);
    }

    hdrLen += std::snprintf(hdr + hdrLen, sizeof(hdr) - hdrLen,
                            "Connection: close\r\n\r\n");

    // Send
    // NOTE: ::send() explicitly calls the WIZnet socket library function,
    // avoiding name shadowing with Webhook::send(const char*).
    auto sendAll = [&](const uint8_t* data, uint32_t n) -> bool {
        uint32_t off = 0;
        while (off < n) {
            int32_t r = ::send(sn, (uint8_t*)data + off, (uint16_t)(n - off));
            if (r <= 0) return false;
            off += (uint32_t)r;
        }
        return true;
    };

    if (!sendAll((const uint8_t*)hdr, (uint32_t)hdrLen)) { close(sn); return false; }
    if (!sendAll((const uint8_t*)payload, (uint32_t)len)) { close(sn); return false; }

    // Read response
    char rx[256];
    int rxUsed = 0;
    uint32_t t0 = HAL_GetTick();
    int httpCode = -1;

    while ((HAL_GetTick() - t0) < 10000) {
        int32_t r = recv(sn, (uint8_t*)rx + rxUsed, (uint16_t)(sizeof(rx) - 1 - rxUsed));
        if (r > 0) {
            rxUsed += (int)r;
            rx[rxUsed] = 0;
            const char* hp = std::strstr(rx, "HTTP/1.");
            if (hp) {
                std::sscanf(hp, "HTTP/%*s %d", &httpCode);
                break;
            }
        }
        HAL_Delay(2);
        IWDG->KR = 0xAAAA;
    }

    disconnect(sn);
    close(sn);

    DBG.info("Webhook: HTTP %d", httpCode);
    return (httpCode >= 200 && httpCode < 300);
}

// ================================================================
// POST via GSM
// ================================================================
bool Webhook::postViaGsm(const char* url, const char* payload, uint16_t len) {
    if (!m_gsm) return false;

    int code = (int)m_gsm->httpPost(url, payload, len);
    DBG.info("Webhook GSM: HTTP %d", code);
    return (code >= 200 && code < 300);
}

// ================================================================
// Send (raw JSON)
// ================================================================
bool Webhook::send(const char* json) {
    if (!isConfigured()) return false;

    uint16_t len = (uint16_t)std::strlen(json);
    const char* url = Cfg().webhook_url;

    // Try W5500 first, then GSM
    if (Cfg().eth_enabled) {
        if (postViaW5500(url, json, len)) return true;
    }
    if (Cfg().gsm_enabled && m_gsm) {
        if (postViaGsm(url, json, len)) return true;
    }

    DBG.error("Webhook: all send methods failed");
    return false;
}

// ================================================================
// Send with template
// ================================================================
bool Webhook::sendTemplated(float value, const char* timestamp, const char* sensorName) {
    if (!isConfigured()) return false;

    const RuntimeConfig& c = Cfg();
    static char payload[512];

    if (c.webhook_payload_template[0]) {
        int len = substituteTemplate(c.webhook_payload_template, payload, sizeof(payload),
                                      value, timestamp, sensorName);
        if (len <= 0) return false;
        return send(payload);
    }

    // Default payload if no template configured
    int len = std::snprintf(payload, sizeof(payload),
        "{\"value\":%.3f,\"timestamp\":\"%s\",\"sensor\":\"%s\"}",
        (double)value, timestamp ? timestamp : "", sensorName ? sensorName : "");
    if (len <= 0 || len >= (int)sizeof(payload)) return false;

    return send(payload);
}

// Estimated Flash: ~2 KB  |  RAM: ~512 bytes (static payload buffer)
