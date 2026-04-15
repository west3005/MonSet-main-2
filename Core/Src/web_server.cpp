/**
 * ================================================================
 * @file web_server.cpp
 * @brief Minimal HTTP server on W5500 socket, port 80.
 *
 * ИСПРАВЛЕНИЕ "Loading..." проблемы:
 *  Причина: handleIndex() отдавал HTML со script fetch('/api/sensors').
 *  Браузер делал второй запрос, но tick() не вызывался 6+ секунд
 *  (главный цикл блокировался Modbus 1с + poll_delay 5с).
 *  Браузер получал таймаут → страница висела на "Loading..."
 *
 *  Решение: handleIndex() теперь отдаёт ПОЛНУЮ самодостаточную HTML-
 *  страницу со всеми данными inline. Вторые запросы не нужны.
 *  <meta refresh=10> обновляет страницу каждые 10 секунд.
 * ================================================================
 */
#include "web_server.hpp"
#include "app.hpp"
#include "circular_log.hpp"
#include "battery_monitor.hpp"
#include "debug_uart.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>

extern "C" {
#include "socket.h"
#include "ff.h"
#include "stm32f4xx_hal.h"
}

extern volatile bool g_web_exclusive;

static const int8_t b64Table[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
};

int WebServer::base64Decode(const char* in, uint8_t* out, int outMax) {
    int len = 0; uint32_t buf = 0; int bits = 0;
    while (*in && len < outMax) {
        char c = *in++;
        if (c == '=') break;
        if (c < 0 || (uint8_t)c >= 128) continue;
        int8_t v = b64Table[(uint8_t)c];
        if (v < 0) continue;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[len++] = (uint8_t)(buf >> bits); }
    }
    return len;
}

WebServer::WebServer() {}

void WebServer::init(SensorReader* sensor, SdBackup* backup, BatteryMonitor* battery) {
    init(sensor, backup, battery, nullptr);
}

void WebServer::init(SensorReader* sensor, SdBackup* backup,
                     BatteryMonitor* battery, App* app) {
    m_sensor  = sensor;
    m_backup  = backup;
    m_battery = battery;
    m_app     = app;
    if (socket(HTTP_SOCKET, Sn_MR_TCP, HTTP_PORT, 0) == HTTP_SOCKET) {
        if (listen(HTTP_SOCKET) == SOCK_OK) {
            m_running = true;
            DBG.info("WebServer: listening on port %u (socket %u)",
                     (unsigned)HTTP_PORT, (unsigned)HTTP_SOCKET);
        }
    }
    if (!m_running) DBG.error("WebServer: failed to start");
}

void WebServer::setActivityCallback(void (*cb)(void*), void* ctx) {
    m_activityCb  = cb;
    m_activityCtx = ctx;
}

static void closeDataSockets(uint8_t httpSock) {
    bool closed = false;
    for (uint8_t i = 0; i < 8; i++) {
        if (i == httpSock) continue;
        uint8_t st = getSn_SR(i);
        if (st != SOCK_CLOSED) {
            disconnect(i);
            HAL_Delay(5);
            close(i);
            closed = true;
        }
    }
    if (closed) DBG.info("WebServer: data sockets closed");
}

bool WebServer::checkAuth(const char* request) {
    const RuntimeConfig& c = Cfg();
    if (c.web_user[0] == 0 && c.web_pass[0] == 0) return true;
    const char* auth = std::strstr(request, "Authorization: Basic ");
    if (!auth) return false;
    auth += 21;
    char b64[128]{}; int i = 0;
    while (auth[i] && auth[i] != '\r' && auth[i] != '\n' && i < 127) b64[i] = auth[i++];
    b64[i] = 0;
    uint8_t decoded[96];
    int dLen = base64Decode(b64, decoded, sizeof(decoded) - 1);
    if (dLen <= 0) return false;
    decoded[dLen] = 0;
    char expected[72];
    std::snprintf(expected, sizeof(expected), "%s:%s", c.web_user, c.web_pass);
    return (std::strcmp((const char*)decoded, expected) == 0);
}

void WebServer::sendResponse(uint8_t sn, int code, const char* contentType,
                              const char* body, uint16_t bodyLen) {
    const char* status = "200 OK";
    if      (code == 401) status = "401 Unauthorized";
    else if (code == 404) status = "404 Not Found";
    else if (code == 400) status = "400 Bad Request";
    char hdr[320]; int hdrLen;
    if (code == 401) {
        hdrLen = std::snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %s\r\n"
            "WWW-Authenticate: Basic realm=\"MonSet\"\r\n"
            "Content-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
            status, contentType, (unsigned)bodyLen);
    } else {
        hdrLen = std::snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %s\r\n"
            "Content-Type: %s\r\nContent-Length: %u\r\n"
            "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
            status, contentType, (unsigned)bodyLen);
    }
    send(sn, (uint8_t*)hdr, (uint16_t)hdrLen);
    if (bodyLen > 0 && body) send(sn, (uint8_t*)body, bodyLen);
}

void WebServer::send401(uint8_t sn) {
    const char* b = "{\"error\":\"Unauthorized\"}";
    sendResponse(sn, 401, "application/json", b, (uint16_t)std::strlen(b));
}
void WebServer::send404(uint8_t sn) {
    const char* b = "{\"error\":\"Not Found\"}";
    sendResponse(sn, 404, "application/json", b, (uint16_t)std::strlen(b));
}

bool WebServer::serveFile(uint8_t sn, const char* path, const char* contentType) {
    char fullPath[64];
    std::snprintf(fullPath, sizeof(fullPath), "0:/www%s", path);
    FIL f;
    if (f_open(&f, fullPath, FA_READ) != FR_OK) return false;
    FSIZE_t fsize = f_size(&f);
    char hdr[192]; int hdrLen = std::snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        contentType, (unsigned long)fsize);
    send(sn, (uint8_t*)hdr, (uint16_t)hdrLen);
    uint8_t chunk[512]; UINT br;
    while (f_read(&f, chunk, sizeof(chunk), &br) == FR_OK && br > 0) {
        send(sn, chunk, (uint16_t)br); IWDG->KR = 0xAAAA;
    }
    f_close(&f);
    return true;
}

// ================================================================
// /api/sensors — JSON (для прямых API запросов)
// ================================================================
void WebServer::handleApiSensors(uint8_t sn) {
    int n = 0;
    n += std::snprintf(m_respBuf + n, RESP_BUF_SIZE - n, "{\"sensors\":[");
    if (m_sensor) {
        for (uint8_t i = 0; i < m_sensor->getReadingCount(); i++) {
            const SensorReading& r = m_sensor->getReading(i);
            if (i > 0) n += std::snprintf(m_respBuf + n, RESP_BUF_SIZE - n, ",");
            n += std::snprintf(m_respBuf + n, RESP_BUF_SIZE - n,
                "{\"name\":\"%s\",\"value\":%.3f,\"raw\":%.3f,\"unit\":\"%s\",\"valid\":%s}",
                r.name, (double)r.value, (double)r.raw_value, r.unit,
                r.valid ? "true" : "false");
        }
    }
    n += std::snprintf(m_respBuf + n, RESP_BUF_SIZE - n, "],");
    if (m_battery) {
        n += std::snprintf(m_respBuf + n, RESP_BUF_SIZE - n,
            "\"battery\":{\"voltage\":%.2f,\"percent\":%u,\"low\":%s}",
            (double)m_battery->getVoltage(), (unsigned)m_battery->getPercent(),
            m_battery->isLow() ? "true" : "false");
    } else {
        n += std::snprintf(m_respBuf + n, RESP_BUF_SIZE - n,
            "\"battery\":{\"voltage\":0,\"percent\":0,\"low\":false}");
    }
    n += std::snprintf(m_respBuf + n, RESP_BUF_SIZE - n, "}");
    sendResponse(sn, 200, "application/json", m_respBuf, (uint16_t)n);
}

// ================================================================
// / (index) — САМОДОСТАТОЧНАЯ HTML страница (все данные inline)
//
// ИСПРАВЛЕНИЕ "Loading...":
//  Старая версия: отдавала HTML с JS fetch('/api/sensors')
//  → браузер делал второй запрос через 50мс
//  → tick() не вызывался 6+ секунд (Modbus 1с + poll 5с)
//  → второй запрос тайм-аут → "Loading..." навсегда
//
//  Новая версия: всё в одном ответе, вторые запросы не нужны.
//  <meta http-equiv="refresh" content="10"> = обновление каждые 10с.
// ================================================================
void WebServer::handleIndex(uint8_t sn) {
    // Приоритет: файл с SD карты (если есть)
    if (serveFile(sn, "/index.html", "text/html")) return;

    // Собираем полную самодостаточную страницу
    char* buf = m_respBuf;
    const int  bsz = RESP_BUF_SIZE;
    int n = 0;

    // --- CSS стили ---
    n += std::snprintf(buf + n, bsz - n,
        "<!DOCTYPE html><html lang='ru'><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='10'>"
        "<title>MonSet Dashboard</title>"
        "<style>"
        "body{font-family:monospace;background:#0d1117;color:#c9d1d9;"
        "margin:0;padding:16px;}"
        "h2{color:#58a6ff;margin-bottom:4px;}"
        ".sub{color:#8b949e;font-size:12px;margin-bottom:16px;}"
        "table{border-collapse:collapse;width:100%%;max-width:600px;"
        "margin-bottom:16px;}"
        "th{background:#161b22;color:#58a6ff;padding:8px 12px;"
        "text-align:left;border:1px solid #30363d;}"
        "td{padding:8px 12px;border:1px solid #30363d;}"
        ".ok{color:#3fb950;font-weight:bold;}"
        ".err{color:#f85149;font-weight:bold;}"
        ".warn{color:#d29922;}"
        ".card{background:#161b22;border:1px solid #30363d;"
        "border-radius:6px;padding:12px;max-width:600px;margin-bottom:12px;}"
        ".label{color:#8b949e;font-size:11px;text-transform:uppercase;"
        "letter-spacing:1px;}"
        ".val{font-size:22px;color:#c9d1d9;margin:4px 0;}"
        ".bat-bar{background:#21262d;border-radius:4px;height:8px;"
        "width:100%%;max-width:200px;margin-top:6px;}"
        ".bat-fill{height:8px;border-radius:4px;background:#3fb950;}"
        "a{color:#58a6ff;text-decoration:none;}"
        "</style></head><body>"
        "<h2>&#127988; MonSet Dashboard</h2>");

    // --- Время (DS3231) ---
    DateTime dt{};
    if (m_app) {
        // получаем время через App если доступно
    }
    n += std::snprintf(buf + n, bsz - n,
        "<div class='sub'>Auto-refresh: 10s &nbsp;|&nbsp; "
        "<a href='/'>&#8635; Refresh now</a></div>");

    // --- Батарея ---
    float bvolt  = m_battery ? m_battery->getVoltage()  : 0.0f;
    uint8_t bpct = m_battery ? m_battery->getPercent()  : 0;
    bool    blow = m_battery ? m_battery->isLow()        : false;
    int fillW = (int)bpct * 200 / 100;
    const char* batCol = blow ? "#f85149" : (bpct < 50 ? "#d29922" : "#3fb950");
    n += std::snprintf(buf + n, bsz - n,
        "<div class='card'>"
        "<div class='label'>Battery</div>"
        "<div class='val'>%.2f V &nbsp; <span class='%s'>%u%%</span></div>"
        "<div class='bat-bar'>"
        "<div class='bat-fill' style='width:%dpx;background:%s;'></div>"
        "</div></div>",
        (double)bvolt,
        blow ? "err" : (bpct < 50 ? "warn" : "ok"),
        (unsigned)bpct,
        fillW, batCol);

    // --- Таблица датчиков ---
    n += std::snprintf(buf + n, bsz - n,
        "<table>"
        "<tr><th>Sensor</th><th>Value</th><th>Unit</th><th>Status</th></tr>");

    if (m_sensor && m_sensor->getReadingCount() > 0) {
        for (uint8_t i = 0; i < m_sensor->getReadingCount(); i++) {
            const SensorReading& r = m_sensor->getReading(i);
            n += std::snprintf(buf + n, bsz - n,
                "<tr><td>%s</td><td>%.3f</td><td>%s</td>"
                "<td class='%s'>%s</td></tr>",
                r.name[0] ? r.name : "sensor",
                (double)r.value,
                r.unit[0] ? r.unit : "—",
                r.valid ? "ok" : "err",
                r.valid ? "OK" : "ERR");
        }
    } else {
        n += std::snprintf(buf + n, bsz - n,
            "<tr><td colspan='4' class='warn'>"
            "No sensors / Modbus timeout</td></tr>");
    }
    n += std::snprintf(buf + n, bsz - n, "</table>");

    // --- Статус каналов ---
    const RuntimeConfig& cfg = Cfg();
    n += std::snprintf(buf + n, bsz - n,
        "<div class='card'>"
        "<div class='label'>Channels</div>"
        "<div style='margin-top:8px;'>"
        "ETH:&nbsp;<span class='%s'>%s</span>&nbsp;&nbsp;"
        "GSM:&nbsp;<span class='%s'>%s</span>&nbsp;&nbsp;"
        "WiFi:&nbsp;<span class='%s'>%s</span>&nbsp;&nbsp;"
        "Iridium:&nbsp;<span class='%s'>%s</span>"
        "</div></div>",
        cfg.eth_enabled      ? "ok"   : "err", cfg.eth_enabled      ? "ON" : "off",
        cfg.gsm_enabled      ? "ok"   : "err", cfg.gsm_enabled      ? "ON" : "off",
        cfg.wifi_enabled     ? "ok"   : "err", cfg.wifi_enabled     ? "ON" : "off",
        cfg.iridium_enabled  ? "ok"   : "err", cfg.iridium_enabled  ? "ON" : "off");

    // --- Web режим ---
    bool webActive = m_app ? m_app->isWebActive() : false;
    uint16_t remaining = m_app ? m_app->webIdleRemainingS() : 0;
    n += std::snprintf(buf + n, bsz - n,
        "<div class='card'>"
        "<div class='label'>Web Mode</div>"
        "<div style='margin-top:6px;'>"
        "Status:&nbsp;<span class='%s'>%s</span>&nbsp;&nbsp;"
        "Idle timeout:&nbsp;<span>%us</span>"
        "</div></div>",
        webActive ? "ok" : "warn",
        webActive ? "ACTIVE (data queued)" : "standby",
        (unsigned)remaining);

    // --- Навигация ---
    n += std::snprintf(buf + n, bsz - n,
        "<div style='margin-top:16px;color:#8b949e;font-size:12px;'>"
        "<a href='/api/sensors'>/api/sensors</a> &nbsp;|&nbsp; "
        "<a href='/api/channels'>/api/channels</a> &nbsp;|&nbsp; "
        "<a href='/config'>/config</a>"
        "</div>"
        "<div style='margin-top:8px;color:#30363d;font-size:11px;'>"
        "MonSet v1.0 &mdash; STM32F407 + W5500</div>"
        "</body></html>");

    if (n < 0 || n >= bsz) n = bsz - 1;
    buf[n] = '\0';
    sendResponse(sn, 200, "text/html", buf, (uint16_t)n);
}

void WebServer::handleConfig(uint8_t sn) {
    if (serveFile(sn, "/config.html", "text/html")) return;
    const RuntimeConfig& c = Cfg();
    int n = std::snprintf(m_respBuf, RESP_BUF_SIZE,
        "{\"poll_interval_sec\":%lu,\"send_interval_polls\":%lu,"
        "\"eth_enabled\":%s,\"gsm_enabled\":%s,\"wifi_enabled\":%s,"
        "\"iridium_enabled\":%s,\"chain_enabled\":%s,"
        "\"mqtt_host\":\"%s\",\"mqtt_port\":%u,"
        "\"protocol\":\"%s\",\"avg_count\":%u}",
        (unsigned long)c.poll_interval_sec, (unsigned long)c.send_interval_polls,
        c.eth_enabled?"true":"false", c.gsm_enabled?"true":"false",
        c.wifi_enabled?"true":"false", c.iridium_enabled?"true":"false",
        c.chain_enabled?"true":"false",
        c.mqtt_host, (unsigned)c.mqtt_port,
        (c.protocol==ProtocolMode::MQTT_GENERIC||
         c.protocol==ProtocolMode::MQTT_THINGSBOARD)?"mqtt":"https",
        (unsigned)c.avg_count);
    sendResponse(sn, 200, "application/json", m_respBuf, (uint16_t)n);
}

void WebServer::handleLogs(uint8_t sn) {
    if (serveFile(sn, "/logs.html", "text/html")) return;
    const char* b = "<html><body><h1>Logs</h1>"
                    "<p>Upload logs.html to SD /www/</p></body></html>";
    sendResponse(sn, 200, "text/html", b, (uint16_t)std::strlen(b));
}

void WebServer::handleExport(uint8_t sn) {
    FIL f;
    if (f_open(&f, Config::BACKUP_FILENAME, FA_READ) != FR_OK) {
        const char* b = "No backup data available";
        sendResponse(sn, 404, "text/plain", b, (uint16_t)std::strlen(b));
        return;
    }
    FSIZE_t fsize = f_size(&f);
    char hdr[256];
    int hdrLen = std::snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Disposition: attachment; filename=\"backup.csv\"\r\n"
        "Content-Length: %lu\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        (unsigned long)fsize);
    send(sn, (uint8_t*)hdr, (uint16_t)hdrLen);
    uint8_t chunk[512]; UINT br;
    while (f_read(&f, chunk, sizeof(chunk), &br) == FR_OK && br > 0) {
        send(sn, chunk, (uint16_t)br); IWDG->KR = 0xAAAA;
    }
    f_close(&f);
}

void WebServer::handlePostConfig(uint8_t sn, const char* body) {
    if (!body || std::strlen(body) < 2) {
        const char* r = "{\"error\":\"empty body\"}";
        sendResponse(sn, 400, "application/json", r, (uint16_t)std::strlen(r)); return;
    }
    RuntimeConfig newCfg = Cfg();
    if (newCfg.loadFromJson(body, std::strlen(body))) {
        Cfg() = newCfg; Cfg().saveToSd(RUNTIME_CONFIG_FILENAME);
        const char* r = "{\"status\":\"ok\",\"message\":\"Saved. Reboot to apply.\"}";
        sendResponse(sn, 200, "application/json", r, (uint16_t)std::strlen(r));
        DBG.info("WebServer: config updated");
    } else {
        const char* r = "{\"error\":\"JSON parse failed\"}";
        sendResponse(sn, 400, "application/json", r, (uint16_t)std::strlen(r));
    }
}

void WebServer::handleApiChannels(uint8_t sn) {
    char buf[512]; const RuntimeConfig& cfg = Cfg();
    uint8_t st = m_app ? m_app->getChannelStatus() : 0;
    int len = std::snprintf(buf, sizeof(buf),
        "{\"eth\":\"%s\",\"gsm\":\"%s\",\"wifi\":\"%s\",\"iridium\":\"%s\","
        "\"tcp_master\":{\"enabled\":%s,\"devices\":%u},"
        "\"tcp_slave\":{\"enabled\":%s,\"port\":%u}}",
        (cfg.channels.eth_enabled)?((st&0x01)?"active":"standby"):"disabled",
        (cfg.channels.gsm_enabled)?((st&0x02)?"active":"standby"):"disabled",
        (cfg.channels.wifi_enabled)?((st&0x04)?"active":"standby"):"disabled",
        (cfg.channels.iridium_enabled)?((st&0x08)?"active":"standby"):"disabled",
        cfg.tcp_master.enabled?"true":"false",(unsigned)cfg.tcp_master.device_count,
        cfg.tcp_slave.enabled?"true":"false",(unsigned)cfg.tcp_slave.listen_port);
    sendResponse(sn, 200, "application/json", buf, (uint16_t)len);
}

void WebServer::handleApiWebMode(uint8_t sn) {
    char buf[128];
    bool active    = m_app ? m_app->isWebActive() : false;
    uint16_t rem   = m_app ? m_app->webIdleRemainingS() : 0;
    int len = std::snprintf(buf, sizeof(buf),
        "{\"web_active\":%s,\"idle_remaining_s\":%u}",
        active?"true":"false", (unsigned)rem);
    sendResponse(sn, 200, "application/json", buf, (uint16_t)len);
}

void WebServer::handleApiTestSend(uint8_t sn) {
    if (m_app) m_app->triggerTestSend();
    const char* r = "{\"ok\":true}";
    sendResponse(sn, 200, "application/json", r, (uint16_t)std::strlen(r));
}

void WebServer::handleApiTestResult(uint8_t sn) {
    char state[16]{}, channel[16]{}; int httpCode = 0; uint32_t elapsed = 0;
    if (m_app) m_app->getTestResult(state, channel, &httpCode, &elapsed);
    else std::strncpy(state, "idle", sizeof(state)-1);
    char buf[256];
    int len = std::snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"channel\":\"%s\",\"http_code\":%d,\"elapsed_ms\":%lu}",
        state, channel, httpCode, (unsigned long)elapsed);
    sendResponse(sn, 200, "application/json", buf, (uint16_t)len);
}

void WebServer::handleApiLogs(uint8_t sn, const char* queryStr) {
    uint32_t offset = 0; uint16_t count = 50;
    if (queryStr && queryStr[0]) {
        const char* p = std::strstr(queryStr, "offset=");
        if (p) offset = (uint32_t)std::atoi(p + 7);
        p = std::strstr(queryStr, "count=");
        if (p) count = (uint16_t)std::atoi(p + 6);
        if (count > 100) count = 100;
    }
    CircularLogBuffer& log = CircularLogBuffer::instance();
    uint16_t total = log.getCount();
    char lineBuf[CircularLogBuffer::LINE_SIZE];
    char resp[RESP_BUF_SIZE];
    int pos = 0;
    pos += std::snprintf(resp + pos, sizeof(resp) - pos,
        "{\"total\":%lu,\"lines\":[", (unsigned long)log.getTotal());
    bool first = true;
    for (uint16_t i = 0; i < count && (offset + i) < total; ++i) {
        if (log.getLine((uint16_t)(offset + i), lineBuf, sizeof(lineBuf))) {
            if (!first && pos < (int)sizeof(resp) - 10) resp[pos++] = ',';
            if (pos < (int)sizeof(resp) - (int)CircularLogBuffer::LINE_SIZE - 4) {
                resp[pos++] = '"';
                for (char* c = lineBuf; *c && pos < (int)sizeof(resp) - 4; ++c) {
                    if (*c == '"') resp[pos++] = '\\';
                    resp[pos++] = *c;
                }
                resp[pos++] = '"'; first = false;
            }
        }
    }
    if (pos < (int)sizeof(resp) - 2) { resp[pos++]=']'; resp[pos++]='}'; resp[pos]='\0'; }
    sendResponse(sn, 200, "application/json", resp, (uint16_t)pos);
}

void WebServer::handleApiLogsExport(uint8_t sn) {
    CircularLogBuffer& log = CircularLogBuffer::instance();
    char lineBuf[CircularLogBuffer::LINE_SIZE];
    char resp[RESP_BUF_SIZE]; int pos = 0;
    uint16_t cnt = log.getCount();
    for (uint16_t i = 0; i < cnt && pos < (int)sizeof(resp) - CircularLogBuffer::LINE_SIZE - 2; ++i) {
        if (log.getLine(i, lineBuf, sizeof(lineBuf))) {
            int n = std::snprintf(resp + pos, sizeof(resp) - pos, "%s\n", lineBuf);
            if (n > 0) pos += n;
        }
    }
    sendResponse(sn, 200, "text/plain", resp, (uint16_t)pos);
}

void WebServer::handleApiLogsClear(uint8_t sn) {
    CircularLogBuffer::instance().clear();
    const char* r = "{\"ok\":true}";
    sendResponse(sn, 200, "application/json", r, (uint16_t)std::strlen(r));
}

void WebServer::handleRequest(uint8_t sn, const char* request, uint16_t reqLen) {
    if (!checkAuth(request)) { send401(sn); return; }
    if (m_activityCb) m_activityCb(m_activityCtx);
    if (m_app) m_app->notifyWebActivity();
    char method[8]{}, path[64]{};
    std::sscanf(request, "%7s %63s", method, path);
    DBG.info("WebServer: %s %s", method, path);
    if (std::strcmp(method, "GET") == 0) {
        if      (std::strcmp(path,"/")==0||std::strcmp(path,"/index.html")==0) handleIndex(sn);
        else if (std::strcmp(path,"/config")==0||std::strcmp(path,"/config.html")==0) handleConfig(sn);
        else if (std::strcmp(path,"/logs")==0||std::strcmp(path,"/logs.html")==0) handleLogs(sn);
        else if (std::strcmp(path,"/export")==0) handleExport(sn);
        else if (std::strcmp(path,"/api/sensors")==0) handleApiSensors(sn);
        else if (std::strcmp(path,"/api/channels")==0) handleApiChannels(sn);
        else if (std::strcmp(path,"/api/web_mode")==0) handleApiWebMode(sn);
        else if (std::strcmp(path,"/api/test_result")==0) handleApiTestResult(sn);
        else if (std::strncmp(path,"/api/logs",9)==0) {
            const char* q = std::strchr(path,'?');
            if (std::strncmp(path,"/api/logs/export",16)==0) handleApiLogsExport(sn);
            else handleApiLogs(sn, q ? q+1 : "");
        }
        else if (std::strcmp(path,"/api/config")==0) handleApiSensors(sn);
        else {
            const char* ext = std::strrchr(path,'.');
            const char* ct = "text/plain";
            if (ext) {
                if      (std::strcmp(ext,".html")==0) ct="text/html";
                else if (std::strcmp(ext,".css")==0)  ct="text/css";
                else if (std::strcmp(ext,".js")==0)   ct="application/javascript";
                else if (std::strcmp(ext,".json")==0) ct="application/json";
            }
            if (!serveFile(sn, path, ct)) send404(sn);
        }
    } else if (std::strcmp(method,"POST")==0) {
        const char* body = std::strstr(request,"\r\n\r\n");
        if (body) body += 4;
        if      (std::strcmp(path,"/config")==0)         handlePostConfig(sn,body);
        else if (std::strcmp(path,"/api/test_send")==0)  handleApiTestSend(sn);
        else if (std::strcmp(path,"/api/logs/clear")==0) handleApiLogsClear(sn);
        else if (std::strcmp(path,"/api/config")==0)     handlePostConfig(sn,body);
        else send404(sn);
    } else { send404(sn); }
}

// ================================================================
// Non-blocking tick — вызывается из App::run()
// ================================================================
void WebServer::tick() {
    if (!m_running) return;
    uint8_t status = getSn_SR(HTTP_SOCKET);
    switch (status) {
        case SOCK_ESTABLISHED: {
            int32_t len = getSn_RX_RSR(HTTP_SOCKET);
            if (len > 0) {
                if (!g_web_exclusive) {
                    g_web_exclusive = true;
                    DBG.info("WebServer: WEB EXCLUSIVE MODE ON");
                    closeDataSockets(HTTP_SOCKET);
                }
                if (len > REQ_BUF_SIZE - 1) len = REQ_BUF_SIZE - 1;
                int32_t received = recv(HTTP_SOCKET, (uint8_t*)m_reqBuf, (uint16_t)len);
                if (received > 0) {
                    m_reqBuf[received] = 0;
                    handleRequest(HTTP_SOCKET, m_reqBuf, (uint16_t)received);
                }
                disconnect(HTTP_SOCKET);
            }
            break;
        }
        case SOCK_CLOSE_WAIT: disconnect(HTTP_SOCKET); break;
        case SOCK_CLOSED:
            if (socket(HTTP_SOCKET, Sn_MR_TCP, HTTP_PORT, 0) == HTTP_SOCKET)
                listen(HTTP_SOCKET);
            break;
        case SOCK_LISTEN: break;
        default: break;
    }
}
