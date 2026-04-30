/**
 * ================================================================
 * @file web_server.hpp
 * @brief Minimal HTTP server on W5500 socket (port 80).
 * ================================================================
 */
#pragma once

#include "runtime_config.hpp"
#include "sensor_reader.hpp"
#include "sd_backup.hpp"
#include <cstdint>

class BatteryMonitor;
class App;

class WebServer {
public:
    WebServer();

    void init(SensorReader* sensor, SdBackup* backup, BatteryMonitor* battery);
    void init(SensorReader* sensor, SdBackup* backup, BatteryMonitor* battery, App* app);

    void tick();
    bool isRunning() const { return m_running; }
    void setActivityCallback(void (*cb)(void*), void* ctx);

    /** Inform the WebServer whether the SD card (FATFS) is available.
     *  Call after App::init() once m_sdOk is known.
     *  When false, POST /api/config applies changes in RAM only. */
    void setSdOk(bool ok) { m_sdOk = ok; }

    /**
     * sendResponse — public.
     * Sends HTTP response with chunked TX to work around W5500
     * 2KB-per-socket TX buffer limit. Without chunking, pages > 2KB
     * are silently truncated by ioLibrary send(), resulting in a
     * blank page in the browser.
     */
    void sendResponse(uint8_t sn, int code, const char* contentType,
                      const char* body, uint16_t bodyLen);

private:
    SensorReader*   m_sensor  = nullptr;
    SdBackup*       m_backup  = nullptr;
    BatteryMonitor* m_battery = nullptr;
    App*            m_app     = nullptr;
    bool            m_running = false;
    bool            m_sdOk    = false;  ///< SD card available (set via setSdOk)

    void (*m_activityCb)(void*) = nullptr;
    void*  m_activityCtx        = nullptr;

    static constexpr uint8_t  HTTP_SOCKET = 5;
    static constexpr uint16_t HTTP_PORT   = 80;

    static constexpr uint16_t REQ_BUF_SIZE  = 4096;
    char m_reqBuf[REQ_BUF_SIZE];

    /**
     * RESP_BUF_SIZE — 6KB вмещает любую inline HTML страницу.
     * TX отправка идёт чанками по TX_CHUNK_SIZE, поэтому буфер
     * может быть больше TX буфера W5500.
     */
    static constexpr uint16_t RESP_BUF_SIZE = 6144;

    /**
     * TX_CHUNK_SIZE — размер одного вызова send() в sendResponse.
     * W5500 по умолчанию 2KB/сокет. Ставим 1024 с запасом.
     * Если увеличишь TX буфер сокета 5 до 4KB в W5500 init —
     * можно поднять до 2048.
     */
    static constexpr uint16_t TX_CHUNK_SIZE = 1024;

    char m_respBuf[RESP_BUF_SIZE];

    bool checkAuth(const char* request);
    void send401(uint8_t sn);
    void send404(uint8_t sn);
    bool serveFile(uint8_t sn, const char* path, const char* contentType);

    void handleRequest(uint8_t sn, const char* request, uint16_t reqLen);

    // HTML pages
    void handleConfig(uint8_t sn);
    void handleIndex(uint8_t sn);
    void handleLogs(uint8_t sn);
    void handleTestPage(uint8_t sn);
    void handleExport(uint8_t sn);

    // JSON API
    void handleApiSensors(uint8_t sn);
    void handleApiConfig(uint8_t sn);
    void handleApiChannels(uint8_t sn);
    void handleApiWebMode(uint8_t sn);
    void handleApiTestSend(uint8_t sn);
    void handleApiTestResult(uint8_t sn);
    void handleApiLogs(uint8_t sn, const char* queryStr);
    void handleApiLogsExport(uint8_t sn);   // ← ДОЛЖЕН БЫТЬ ОПРЕДЕЛЁН В CPP
    void handleApiLogsClear(uint8_t sn);
    void handleApiBackupDownload(uint8_t sn);

    // POST
    void handlePostConfig(uint8_t sn, const char* body);

    static int base64Decode(const char* in, uint8_t* out, int outMax);
};
