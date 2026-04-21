/**
 * ================================================================
 * @file web_server.cpp
 * @brief Minimal HTTP server on W5500 (port 80).
 *        All pages are inline HTML — SD card not required.
 * ================================================================
 */
#include "web_server.hpp"
#include "app.hpp"
#include "circular_log.hpp"
#include "battery_monitor.hpp"
#include "debug_uart.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "socket.h"
#include "ff.h"
#include "stm32f4xx_hal.h"
}

extern volatile bool g_web_exclusive;

// ── Base64 ────────────────────────────────────────────────────────────────────
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
    int len=0; uint32_t buf=0; int bits=0;
    while(*in && len<outMax){
        char c=*in++;
        if(c=='=') break;
        if((uint8_t)c>=128) continue;
        int8_t v=b64Table[(uint8_t)c]; if(v<0) continue;
        buf=(buf<<6)|(uint32_t)v; bits+=6;
        if(bits>=8){bits-=8; out[len++]=(uint8_t)(buf>>bits);}
    }
    return len;
}

// ── Shared CSS (темная тема, навигация) ───────────────────────────────────────
static const char CSS[] =
    "body{font-family:monospace;background:#0d1117;color:#c9d1d9;margin:0;padding:16px}"
    "h2{color:#58a6ff;margin:0 0 12px}"
    "nav{background:#161b22;border:1px solid #30363d;border-radius:6px;"
    "padding:8px 14px;margin-bottom:18px;display:flex;gap:4px;flex-wrap:wrap}"
    "nav a{color:#8b949e;text-decoration:none;padding:4px 10px;border-radius:4px;font-size:13px}"
    "nav a:hover{color:#c9d1d9;background:#21262d}"
    "nav a.active{color:#58a6ff;background:#21262d}"
    ".card{background:#161b22;border:1px solid #30363d;border-radius:6px;"
    "padding:16px;max-width:640px;margin-bottom:14px}"
    "h3{color:#8b949e;font-size:11px;text-transform:uppercase;letter-spacing:.8px;margin:0 0 10px}"
    "label{display:block;color:#8b949e;font-size:11px;margin-bottom:3px}"
    "input[type=text],input[type=number],input[type=password]{"
    "background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:4px;"
    "padding:6px 8px;width:100%;box-sizing:border-box;margin-bottom:10px;"
    "font-family:monospace;font-size:13px}"
    "input[type=checkbox]{width:auto;margin:0 6px 0 0;accent-color:#238636}"
    ".row{display:flex;gap:12px}.row>div{flex:1}"
    ".btn{background:#238636;color:#fff;border:none;border-radius:6px;"
    "padding:8px 18px;cursor:pointer;font-family:monospace;font-size:13px}"
    ".btn:hover{background:#2ea043}"
    ".btn-danger{background:#b62324}.btn-danger:hover{background:#d73a3a}"
    "#msg{margin-top:8px;min-height:18px;font-size:12px}"
    ".ok{color:#3fb950}.err{color:#f85149}.warn{color:#d29922}"
    "table{border-collapse:collapse;width:100%;max-width:640px;margin-bottom:14px}"
    "th{background:#161b22;color:#58a6ff;padding:8px 12px;text-align:left;"
    "border:1px solid #30363d}"
    "td{padding:7px 12px;border:1px solid #30363d}"
    ".tag{display:inline-block;padding:2px 8px;border-radius:10px;font-size:11px;"
    "background:#21262d}"
    "footer{color:#484f58;font-size:11px;margin-top:16px}";

// ── Macro helpers ─────────────────────────────────────────────────────────────
#define SNCAT(fmt, ...) \
    do { if (n < bsz-1) n += std::snprintf(buf+n, bsz-n, fmt, ##__VA_ARGS__); } while(0)

static void writeHead(char* buf, int& n, int bsz, const char* title) {
    n += std::snprintf(buf+n, bsz-n,
        "<!DOCTYPE html><html lang='ru'><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%s &mdash; MonSet</title>"
        "<style>%s</style>"
        "</head><body>",
        title, CSS);
}
static void writeNav(char* buf, int& n, int bsz, const char* active) {
    n += std::snprintf(buf+n, bsz-n,
        "<nav>"
        "<a href='/' class='%s'>&#9881; Config</a>"
        "<a href='/dashboard' class='%s'>&#128200; Dashboard</a>"
        "<a href='/logs' class='%s'>&#128221; Logs</a>"
        "<a href='/test' class='%s'>&#9654; Test</a>"
        "</nav>",
        std::strcmp(active,"config")==0    ? "active" : "",
        std::strcmp(active,"dashboard")==0 ? "active" : "",
        std::strcmp(active,"logs")==0      ? "active" : "",
        std::strcmp(active,"test")==0      ? "active" : "");
}

// ── Constructor / init ────────────────────────────────────────────────────────
WebServer::WebServer() {}

void WebServer::init(SensorReader* s, SdBackup* b, BatteryMonitor* bat) {
    init(s, b, bat, nullptr);
}
void WebServer::init(SensorReader* s, SdBackup* b, BatteryMonitor* bat, App* app) {
    m_sensor=s; m_backup=b; m_battery=bat; m_app=app;
    if(socket(HTTP_SOCKET,Sn_MR_TCP,HTTP_PORT,0)==HTTP_SOCKET) {
        if(listen(HTTP_SOCKET)==SOCK_OK) {
            m_running=true;
            DBG.info("WebServer: listening on port %u (socket %u)",
                     (unsigned)HTTP_PORT,(unsigned)HTTP_SOCKET);
        }
    }
    if(!m_running) DBG.error("WebServer: failed to start");
}
void WebServer::setActivityCallback(void(*cb)(void*),void* ctx){m_activityCb=cb;m_activityCtx=ctx;}

// ── Utility ───────────────────────────────────────────────────────────────────
static void closeDataSockets(uint8_t httpSock){
    bool closed=false;
    for(uint8_t i=0;i<8;i++){
        if(i==httpSock) continue;
        if(getSn_SR(i)!=SOCK_CLOSED){disconnect(i);HAL_Delay(5);close(i);closed=true;}
    }
    if(closed) DBG.info("WebServer: data sockets closed");
}
bool WebServer::checkAuth(const char* req){
    const RuntimeConfig& c=Cfg();
    if(c.web_user[0]==0&&c.web_pass[0]==0) return true;
    const char* auth=std::strstr(req,"Authorization: Basic ");
    if(!auth) return false;
    auth+=21;
    char b64[128]{}; int i=0;
    while(auth[i]&&auth[i]!='\r'&&auth[i]!='\n'&&i<127) b64[i]=auth[i++];
    uint8_t dec[96]; int dLen=base64Decode(b64,dec,sizeof(dec)-1);
    if(dLen<=0) return false;
    dec[dLen]=0;
    char exp[72]; std::snprintf(exp,sizeof(exp),"%s:%s",c.web_user,c.web_pass);
    return std::strcmp((const char*)dec,exp)==0;
}
void WebServer::sendResponse(uint8_t sn, int code, const char* ct,
                              const char* body, uint16_t bodyLen)
{
    const char* status = "200 OK";
    if (code == 400) status = "400 Bad Request";
    else if (code == 401) status = "401 Unauthorized";
    else if (code == 404) status = "404 Not Found";

    char hdr[320];
    int hlen;
    if (code == 401) {
        hlen = std::snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %s\r\nWWW-Authenticate: Basic realm=\"MonSet\"\r\n"
            "Content-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
            status, ct, (unsigned)bodyLen);
    } else {
        hlen = std::snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
            "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
            status, ct, (unsigned)bodyLen);
    }
    send(sn, (uint8_t*)hdr, (uint16_t)hlen);

    /* ── Chunked TX to handle W5500 2KB/socket TX buffer limit ── */
    uint16_t offset = 0;
    while (offset < bodyLen && body) {
        uint16_t chunk = bodyLen - offset;
        if (chunk > TX_CHUNK_SIZE) chunk = TX_CHUNK_SIZE;
        int32_t r = send(sn, (uint8_t*)(body + offset), chunk);
        if (r <= 0) break;
        offset += (uint16_t)r;
        IWDG->KR = 0xAAAA;   // кормим watchdog при больших страницах
    }
}
void WebServer::send401(uint8_t sn){
    const char* b="{\"error\":\"Unauthorized\"}";
    sendResponse(sn,401,"application/json",b,(uint16_t)std::strlen(b));
}
void WebServer::send404(uint8_t sn){
    const char* b="{\"error\":\"Not Found\"}";
    sendResponse(sn,404,"application/json",b,(uint16_t)std::strlen(b));
}
bool WebServer::serveFile(uint8_t sn,const char* path,const char* ct){
    char fp[64]; std::snprintf(fp,sizeof(fp),"0:/www%s",path);
    FIL f;
    if(f_open(&f,fp,FA_READ)!=FR_OK) return false;
    FSIZE_t fsize=f_size(&f);
    char hdr[192];
    int hl=std::snprintf(hdr,sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        ct,(unsigned long)fsize);
    send(sn,(uint8_t*)hdr,(uint16_t)hl);
    uint8_t chunk[512]; UINT br;
    while(f_read(&f,chunk,sizeof(chunk),&br)==FR_OK&&br>0){
        send(sn,chunk,(uint16_t)br); IWDG->KR=0xAAAA;
    }
    f_close(&f); return true;
}

// ── GET / and GET /config ─────────────────────────────────────────────────────
void WebServer::handleConfig(uint8_t sn){
    if(serveFile(sn,"/config.html","text/html")) return;
    char* buf=m_respBuf; const int bsz=RESP_BUF_SIZE; int n=0;
    const RuntimeConfig& c=Cfg();

    writeHead(buf,n,bsz,"Config");
    SNCAT("<h2>&#9881; MonSet Configuration</h2>");
    writeNav(buf,n,bsz,"config");

    // Server card
    SNCAT("<div class='card'><h3>Server</h3>"
          "<label>Server URL</label>"
          "<input type='text' id='url' value='%s'>",c.server_url);
    SNCAT("<div class='row'>"
          "<div><label>Poll interval (s)</label>"
          "<input type='number' id='poll' min='1' value='%lu'></div>"
          "<div><label>Send each N polls</label>"
          "<input type='number' id='each' min='1' value='%lu'></div></div>",
          (unsigned long)c.poll_interval_sec,(unsigned long)c.send_interval_polls);

    // Channels card
    SNCAT("<h3>Channels</h3>"
          "<label><input type='checkbox' id='eth' %s> ETH (W5500)</label>"
          "<label><input type='checkbox' id='gsm' %s> GSM (A7670E)</label>"
          "<label><input type='checkbox' id='wifi' %s> WiFi (ESP8266)</label>"
          "<label><input type='checkbox' id='ird' %s> Iridium</label>",
          c.eth_enabled?"checked":"",
          c.gsm_enabled?"checked":"",
          c.wifi_enabled?"checked":"",
          c.iridium_enabled?"checked":"");

    SNCAT("<br><br>"
          "<button class='btn' onclick='doSave()'>&#128190; Save</button>"
          "<div id='msg'></div></div>");

    // Runtime info — FIX: correct field names from runtime_config.hpp
    // eth_ip is uint8_t[4], format as x.x.x.x
    // modbus fields: modbus_slave, modbus_func, modbus_start_reg, modbus_num_regs
    // protocol: ProtocolMode::HTTPS_THINGSBOARD (not HTTPS)
    SNCAT("<div class='card'><h3>Runtime Info</h3><table>"
          "<tr><td style='color:#8b949e'>Metric ID</td>"
          "<td style='font-size:12px'>%s</td></tr>"
          "<tr><td style='color:#8b949e'>ETH IP</td>"
          "<td>%u.%u.%u.%u</td></tr>"
          "<tr><td style='color:#8b949e'>Protocol</td><td>%s</td></tr>"
          "<tr><td style='color:#8b949e'>Modbus</td>"
          "<td>slave=%u fc=%u reg=%u n=%u</td></tr>"
          "</table></div>",
          c.metric_id,
          (unsigned)c.eth_ip[0],(unsigned)c.eth_ip[1],
          (unsigned)c.eth_ip[2],(unsigned)c.eth_ip[3],
          (c.protocol==ProtocolMode::HTTPS_THINGSBOARD) ? "HTTPS" :
          (c.protocol==ProtocolMode::MQTT_THINGSBOARD)  ? "MQTT-TB" :
          (c.protocol==ProtocolMode::MQTT_GENERIC)       ? "MQTT" : "Webhook",
          (unsigned)c.modbus_slave,
          (unsigned)c.modbus_func,
          (unsigned)c.modbus_start_reg,
          (unsigned)c.modbus_num_regs);

    // JS save handler
    SNCAT("<script>"
          "function doSave(){"
          "var el=document.getElementById('msg');"
          "el.textContent='Saving...';el.className='';"
          "fetch('/config',{method:'POST',"
          "headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({"
          "server_url:document.getElementById('url').value,"
          "poll_interval_sec:parseInt(document.getElementById('poll').value)||5,"
          "send_interval_polls:parseInt(document.getElementById('each').value)||2,"
          "eth_enabled:document.getElementById('eth').checked,"
          "gsm_enabled:document.getElementById('gsm').checked,"
          "wifi_enabled:document.getElementById('wifi').checked,"
          "iridium_enabled:document.getElementById('ird').checked"
          "})})"
          ".then(r=>r.json())"
          ".then(j=>{el.textContent=j.message||j.error||'OK';"
          "el.className=j.error?'err':'ok';})"
          ".catch(e=>{el.textContent='Error: '+e;el.className='err';});"
          "}</script>"
          "<footer>MonSet v1.0 &mdash; STM32F407 + W5500</footer>"
          "</body></html>");

    if(n>=bsz) n=bsz-1; buf[n]='\0';
    sendResponse(sn,200,"text/html; charset=UTF-8",buf,(uint16_t)n);
}

// ── GET /dashboard ────────────────────────────────────────────────────────────
void WebServer::handleIndex(uint8_t sn){
    if(serveFile(sn,"/dashboard.html","text/html")) return;
    if(serveFile(sn,"/index.html","text/html")) return;
    char* buf=m_respBuf; const int bsz=RESP_BUF_SIZE; int n=0;
    const RuntimeConfig& cfg=Cfg();

    writeHead(buf,n,bsz,"Dashboard");
    SNCAT("<h2>&#128200; MonSet Dashboard</h2>");
    writeNav(buf,n,bsz,"dashboard");

    // Battery
    float bvolt=m_battery?m_battery->getVoltage():0.0f;
    uint8_t bpct=m_battery?m_battery->getPercent():0;
    bool blow=m_battery?m_battery->isLow():false;
    int fpx=(int)bpct*180/100;
    SNCAT("<div class='card'><h3>Battery</h3>"
          "<div class='%s' style='font-size:22px;margin-bottom:6px'>%.2f V &nbsp; %u%%</div>"
          "<div style='background:#21262d;border-radius:4px;height:8px;width:180px'>"
          "<div style='width:%dpx;height:8px;border-radius:4px;background:%s'></div>"
          "</div></div>",
          blow?"err":(bpct<50?"warn":"ok"),(double)bvolt,(unsigned)bpct,
          fpx,blow?"#f85149":(bpct<50?"#d29922":"#3fb950"));

    // Sensors
    SNCAT("<table>"
          "<tr><th>Sensor</th><th>Value</th><th>Unit</th><th>Status</th></tr>");
    if(m_sensor&&m_sensor->getReadingCount()>0){
        for(uint8_t i=0;i<m_sensor->getReadingCount();i++){
            const SensorReading& r=m_sensor->getReading(i);
            SNCAT("<tr><td>%s</td><td>%.3f</td><td>%s</td>"
                  "<td><span class='tag %s'>%s</span></td></tr>",
                  r.name[0]?r.name:"sensor",(double)r.value,
                  r.unit[0]?r.unit:"—",r.valid?"ok":"err",r.valid?"OK":"ERR");
        }
    }else{
        SNCAT("<tr><td colspan='4' style='color:#8b949e'>No sensor data / Modbus timeout</td></tr>");
    }
    SNCAT("</table>");

    // Channels
    SNCAT("<div class='card'><h3>Channels</h3>"
          "<span class='tag %s' style='margin-right:8px'>ETH %s</span>"
          "<span class='tag %s' style='margin-right:8px'>GSM %s</span>"
          "<span class='tag %s' style='margin-right:8px'>WiFi %s</span>"
          "<span class='tag %s'>Iridium %s</span></div>",
          cfg.eth_enabled?"ok":"err",cfg.eth_enabled?"ON":"off",
          cfg.gsm_enabled?"ok":"err",cfg.gsm_enabled?"ON":"off",
          cfg.wifi_enabled?"ok":"err",cfg.wifi_enabled?"ON":"off",
          cfg.iridium_enabled?"ok":"err",cfg.iridium_enabled?"ON":"off");

    SNCAT("<footer>"
          "<a href='javascript:location.reload()'>&#8635; Refresh</a>"
          " &mdash; MonSet v1.0</footer></body></html>");
    if(n>=bsz) n=bsz-1; buf[n]='\0';
    sendResponse(sn,200,"text/html; charset=UTF-8",buf,(uint16_t)n);
}

// ── GET /logs ─────────────────────────────────────────────────────────────────
void WebServer::handleLogs(uint8_t sn){
    if(serveFile(sn,"/logs.html","text/html")) return;
    char* buf=m_respBuf; const int bsz=RESP_BUF_SIZE; int n=0;
    CircularLogBuffer& log=CircularLogBuffer::instance();
    uint16_t total=log.getCount();

    writeHead(buf,n,bsz,"Logs");
    SNCAT("<h2>&#128221; System Logs <span style='color:#8b949e;font-size:14px'>(%u)</span></h2>",
          (unsigned)total);
    writeNav(buf,n,bsz,"logs");

    SNCAT("<div style='margin-bottom:10px;display:flex;gap:8px'>"
          "<button class='btn' onclick='location.reload()'>&#8635; Refresh</button>"
          "<button class='btn btn-danger' onclick='clrLogs()'>&#128465; Clear</button>"
          "<a href='/api/logs/export' style='color:#58a6ff;font-size:13px;"
          "padding:8px 14px;background:#161b22;border:1px solid #30363d;"
          "border-radius:6px;text-decoration:none'>&#11123; Export</a></div>");

    SNCAT("<div style='background:#161b22;border:1px solid #30363d;border-radius:6px;"
          "padding:8px;max-height:65vh;overflow-y:auto;font-size:12px'>");
    char line[CircularLogBuffer::LINE_SIZE];
    for(uint16_t i=0;i<total&&n<bsz-200;i++){
        if(!log.getLine(i,line,sizeof(line))) continue;
        const char* cls="";
        if(std::strstr(line,"[ERROR]")) cls="err";
        else if(std::strstr(line,"[WARN ]")) cls="warn";
        else if(std::strstr(line,"[DATA ]")) cls="ok";
        n+=std::snprintf(buf+n,bsz-n,
            "<div style='padding:1px 4px;border-bottom:1px solid #21262d'>"
            "<span class='%s'>",cls);
        for(const char* p=line;*p&&n<bsz-12;p++){
            if(*p=='<'){std::memcpy(buf+n,"&lt;",4);n+=4;}
            else if(*p=='>'){std::memcpy(buf+n,"&gt;",4);n+=4;}
            else if(*p=='&'){std::memcpy(buf+n,"&amp;",5);n+=5;}
            else buf[n++]=*p;
        }
        if(n<bsz-16){std::memcpy(buf+n,"</span></div>",13);n+=13;}
    }
    SNCAT("</div>"
          "<script>function clrLogs(){"
          "fetch('/api/logs/clear',{method:'POST'}).then(()=>location.reload());}"
          "</script>"
          "<footer style='margin-top:10px'>RAM circular buffer</footer>"
          "</body></html>");
    if(n>=bsz) n=bsz-1; buf[n]='\0';
    sendResponse(sn,200,"text/html; charset=UTF-8",buf,(uint16_t)n);
}

// ── GET /test ─────────────────────────────────────────────────────────────────
void WebServer::handleTestPage(uint8_t sn){
    char* buf=m_respBuf; const int bsz=RESP_BUF_SIZE; int n=0;
    writeHead(buf,n,bsz,"Test Send");
    SNCAT("<h2>&#9654; Test Send</h2>");
    writeNav(buf,n,bsz,"test");
    SNCAT("<div class='card'><h3>Manual Trigger</h3>"
          "<p style='color:#8b949e;font-size:13px;margin-bottom:14px'>"
          "Send one data packet to the configured server URL immediately.</p>"
          "<button class='btn' onclick='doTest()'>&#9654; Send Now</button>"
          "<div id='res' style='margin-top:14px;padding:10px;border-radius:4px;"
          "background:#21262d;min-height:36px;font-size:13px'>Ready.</div></div>"
          "<div class='card'><h3>Target URL</h3>"
          "<div style='font-size:12px;color:#8b949e;word-break:break-all'>%s</div></div>",
          Cfg().server_url);
    SNCAT("<script>"
          "var t;"
          "function doTest(){"
          "if(t)clearInterval(t);"
          "var el=document.getElementById('res');"
          "el.textContent='Sending...';el.className='';"
          "fetch('/api/test_send',{method:'POST'})"
          ".then(()=>{t=setInterval(poll,1200);})"
          ".catch(e=>{el.textContent='Error: '+e;el.className='err';});}"
          "function poll(){"
          "fetch('/api/test_result').then(r=>r.json()).then(j=>{"
          "var el=document.getElementById('res');"
          "if(j.status==='running'){el.textContent='Running via '+j.channel+'...';return;}"
          "if(j.status==='idle')return;"
          "clearInterval(t);"
          "var ok=j.http_code>=200&&j.http_code<300;"
          "el.className=ok?'ok':'err';"
          "el.textContent=j.status+' | '+j.channel+' | HTTP '+j.http_code+' | '+j.elapsed_ms+'ms';"
          "}).catch(()=>{});}"
          "</script>"
          "<footer>MonSet v1.0</footer></body></html>");
    if(n>=bsz) n=bsz-1; buf[n]='\0';
    sendResponse(sn,200,"text/html; charset=UTF-8",buf,(uint16_t)n);
}

// ── GET /export ───────────────────────────────────────────────────────────────
void WebServer::handleExport(uint8_t sn){
    FIL f;
    if(f_open(&f,Config::BACKUP_FILENAME,FA_READ)!=FR_OK){
        const char* b="No backup data available";
        sendResponse(sn,404,"text/plain",b,(uint16_t)std::strlen(b)); return;
    }
    FSIZE_t fsize=f_size(&f);
    char hdr[256];
    int hl=std::snprintf(hdr,sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
        "Content-Disposition: attachment; filename=\"backup.csv\"\r\n"
        "Content-Length: %lu\r\nAccess-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",(unsigned long)fsize);
    send(sn,(uint8_t*)hdr,(uint16_t)hl);
    uint8_t chunk[512]; UINT br;
    while(f_read(&f,chunk,sizeof(chunk),&br)==FR_OK&&br>0){
        send(sn,chunk,(uint16_t)br); IWDG->KR=0xAAAA;
    }
    f_close(&f);
}

// ── GET /api/sensors ──────────────────────────────────────────────────────────
void WebServer::handleApiSensors(uint8_t sn){
    int n=0;
    n+=std::snprintf(m_respBuf+n,RESP_BUF_SIZE-n,"{\"sensors\":[");
    if(m_sensor){
        for(uint8_t i=0;i<m_sensor->getReadingCount();i++){
            const SensorReading& r=m_sensor->getReading(i);
            if(i>0) n+=std::snprintf(m_respBuf+n,RESP_BUF_SIZE-n,",");
            n+=std::snprintf(m_respBuf+n,RESP_BUF_SIZE-n,
                "{\"name\":\"%s\",\"value\":%.3f,\"raw\":%.3f,"
                "\"unit\":\"%s\",\"valid\":%s}",
                r.name,(double)r.value,(double)r.raw_value,
                r.unit,r.valid?"true":"false");
        }
    }
    n+=std::snprintf(m_respBuf+n,RESP_BUF_SIZE-n,"],");
    if(m_battery){
        n+=std::snprintf(m_respBuf+n,RESP_BUF_SIZE-n,
            "\"battery\":{\"voltage\":%.2f,\"percent\":%u,\"low\":%s}",
            (double)m_battery->getVoltage(),(unsigned)m_battery->getPercent(),
            m_battery->isLow()?"true":"false");
    }else{
        n+=std::snprintf(m_respBuf+n,RESP_BUF_SIZE-n,
            "\"battery\":{\"voltage\":0,\"percent\":0,\"low\":false}");
    }
    n+=std::snprintf(m_respBuf+n,RESP_BUF_SIZE-n,"}");
    sendResponse(sn,200,"application/json",m_respBuf,(uint16_t)n);

    uint32_t tick = HAL_GetTick() / 1000;
    n+=std::snprintf(m_respBuf+n,RESP_BUF_SIZE-n,
        ",\"uptime_s\":%lu,\"timestamp\":\"uptime %lu s\"",
        (unsigned long)tick,(unsigned long)tick);
}

// ── GET /api/config (JSON) ────────────────────────────────────────────────────
// FIX: ProtocolMode::HTTPS → ProtocolMode::HTTPS_THINGSBOARD
void WebServer::handleApiConfig(uint8_t sn){
    const RuntimeConfig& c = Cfg();

    char sip[16],ssn[16],sgw[16],sdns[16];
    std::snprintf(sip,16,"%u.%u.%u.%u",c.eth_ip[0],c.eth_ip[1],c.eth_ip[2],c.eth_ip[3]);
    std::snprintf(ssn,16,"%u.%u.%u.%u",c.eth_sn[0],c.eth_sn[1],c.eth_sn[2],c.eth_sn[3]);
    std::snprintf(sgw,16,"%u.%u.%u.%u",c.eth_gw[0],c.eth_gw[1],c.eth_gw[2],c.eth_gw[3]);
    std::snprintf(sdns,16,"%u.%u.%u.%u",c.eth_dns[0],c.eth_dns[1],c.eth_dns[2],c.eth_dns[3]);

    const char* protoStr =
        (c.protocol==ProtocolMode::HTTPS_THINGSBOARD)?"https_tb":
        (c.protocol==ProtocolMode::MQTT_THINGSBOARD) ?"mqtt_tb" :
        (c.protocol==ProtocolMode::MQTT_GENERIC)     ?"mqtt_gen":
        (c.protocol==ProtocolMode::WEBHOOK_HTTP)     ?"webhook" :"https_tb";

    const char* web_user = c.web.web_user[0] ? c.web.web_user : c.web_user;
    const char* ntp_srv  = c.time_cfg.ntp_server[0] ? c.time_cfg.ntp_server : c.ntp_host;
    bool  ch_eth  = c.channels.eth_enabled      || c.eth_enabled;
    bool  ch_gsm  = c.channels.gsm_enabled      || c.gsm_enabled;
    bool  ch_wifi = c.channels.wifi_enabled     || c.wifi_enabled;
    bool  ch_irid = c.channels.iridium_enabled  || c.iridium_enabled;
    bool  chain   = c.channels.chain_mode       || c.chain_enabled;
    uint32_t poll_s = c.meas.poll_interval_s  ? (uint32_t)c.meas.poll_interval_s  : c.poll_interval_sec;
    uint32_t send_s = c.meas.send_interval_s  ? (uint32_t)c.meas.send_interval_s  : (c.send_interval_polls*c.poll_interval_sec);
    uint16_t bkup_s = c.meas.backup_retry_s   ? c.meas.backup_retry_s : (uint16_t)c.backup_send_interval_sec;
    uint8_t  avg    = c.meas.avg_count ? c.meas.avg_count : c.avg_count;
    const char* tb_host = c.proto.tb_host[0]   ? c.proto.tb_host   : "thingsboard.cloud";
    const char* mq_host = c.proto.mqtt_host[0] ? c.proto.mqtt_host  : c.mqtt_host;
    uint16_t mq_port    = c.proto.mqtt_port    ? c.proto.mqtt_port  : c.mqtt_port;
    const char* mq_user = c.proto.mqtt_user[0]  ? c.proto.mqtt_user  : c.mqtt_user;
    const char* mq_top  = c.proto.mqtt_topic[0] ? c.proto.mqtt_topic : c.mqtt_topic;
    const char* wh_url  = c.proto.webhook_url[0]    ? c.proto.webhook_url    : c.webhook_url;
    const char* wh_meth = c.proto.webhook_method[0] ? c.proto.webhook_method : c.webhook_method;
    bool eth_dhcp = (c.eth_mode == RuntimeConfig::EthMode::Dhcp);
    float bat_low = c.alerts.battery_low_threshold_pct>0 ?
                    c.alerts.battery_low_threshold_pct : (float)c.battery_low_pct;

    char cord[32];
    std::snprintf(cord,sizeof(cord),"[%u,%u,%u,%u]",
        c.channels.chain_order[0],c.channels.chain_order[1],
        c.channels.chain_order[2],c.channels.chain_order[3]);

    int n=0;
    n+=std::snprintf(m_respBuf+n,RESP_BUF_SIZE-n,
        "{"
        "\"server_url\":\"%s\","
        "\"poll_interval_sec\":%lu,\"send_interval_polls\":%lu,"
        "\"ch_eth\":%s,\"ch_gsm\":%s,\"ch_wifi\":%s,\"ch_iridium\":%s,"
        "\"eth_enabled\":%s,\"gsm_enabled\":%s,\"wifi_enabled\":%s,\"iridium_enabled\":%s,"
        "\"chain_mode\":%s,\"chain_order\":%s,"
        "\"channel_timeout_s\":%u,\"channel_retry_s\":%u,\"channel_max_retries\":%u,"
        "\"proto\":\"%s\","
        "\"tb_host\":\"%s\",\"tb_token\":\"%s\",\"tb_port\":%u,"
        "\"mqtt_host\":\"%s\",\"mqtt_port\":%u,\"mqtt_user\":\"%s\","
        "\"mqtt_topic\":\"%s\",\"mqtt_qos\":%u,\"mqtt_tls\":%s,"
        "\"webhook_url\":\"%s\",\"webhook_method\":\"%s\","
        "\"poll_interval_s\":%lu,\"send_interval_s\":%lu,"
        "\"backup_retry_s\":%u,\"avg_count\":%u,"
        "\"deep_sleep_enabled\":%s,\"deep_sleep_s\":%u,"
        "\"schedule_enabled\":%s,\"schedule_start\":\"%s\",\"schedule_stop\":\"%s\","
        "\"eth_dhcp\":%s,"
        "\"eth_ip\":\"%s\",\"eth_sn\":\"%s\",\"eth_gw\":\"%s\",\"eth_dns\":\"%s\","
        "\"gsm_apn\":\"%s\",\"gsm_user\":\"%s\","
        "\"wifi_ssid\":\"%s\","
        "\"ntp_enabled\":%s,\"ntp_server\":\"%s\",\"tz_off\":%d,"
        "\"web_user\":\"%s\",\"web_port\":%u,\"web_idle_timeout_s\":%u,"
        "\"web_auth_enabled\":%s,\"web_exclusive_mode\":%s,"
        "\"mtcpm_en\":%s,\"mtcps_en\":%s,"
        "\"sl_port\":%u,\"sl_uid\":%u,\"sl_sock\":%u,\"sl_ctms\":%u,"
        "\"alerts_enabled\":%s,\"batt_low\":%.1f,"
        "\"alert_on_channel_fail\":%s,\"alert_on_sensor_fail\":%s,"
        "\"alert_webhook_url\":\"%s\""
        "}",
        c.server_url,
        (unsigned long)c.poll_interval_sec,(unsigned long)c.send_interval_polls,
        ch_eth?"true":"false",ch_gsm?"true":"false",ch_wifi?"true":"false",ch_irid?"true":"false",
        ch_eth?"true":"false",ch_gsm?"true":"false",ch_wifi?"true":"false",ch_irid?"true":"false",
        chain?"true":"false",cord,
        (unsigned)c.channels.channel_timeout_s,(unsigned)c.channels.channel_retry_s,
        (unsigned)c.channels.channel_max_retries,
        protoStr,
        tb_host,c.proto.tb_token,(unsigned)c.proto.tb_port,
        mq_host,(unsigned)mq_port,mq_user,
        mq_top,(unsigned)c.proto.mqtt_qos,(c.proto.mqtt_tls||c.mqtt_tls)?"true":"false",
        wh_url,wh_meth,
        (unsigned long)poll_s,(unsigned long)send_s,
        (unsigned)bkup_s,(unsigned)avg,
        c.meas.deep_sleep_enabled?"true":"false",(unsigned)c.meas.deep_sleep_s,
        c.meas.schedule_enabled?"true":"false",c.meas.schedule_start,c.meas.schedule_stop,
        eth_dhcp?"true":"false",
        sip,ssn,sgw,sdns,
        c.gsm_apn,c.gsm_user,c.wifi_ssid,
        c.time_cfg.ntp_enabled?"true":"false",ntp_srv,(int)c.time_cfg.timezone_offset,
        web_user,(unsigned)c.web.web_port,(unsigned)c.web.web_idle_timeout_s,
        c.web.web_auth_enabled?"true":"false",c.web.web_exclusive_mode?"true":"false",
        c.tcp_master.enabled?"true":"false",c.tcp_slave.enabled?"true":"false",
        (unsigned)c.tcp_slave.listen_port,(unsigned)c.tcp_slave.unit_id,
        (unsigned)c.tcp_slave.w5500_socket,(unsigned)c.tcp_slave.connection_timeout_ms,
        c.alerts.alerts_enabled?"true":"false",(double)bat_low,
        c.alerts.alert_on_channel_fail?"true":"false",
        c.alerts.alert_on_sensor_fail?"true":"false",
        c.alerts.alert_webhook_url
    );
    sendResponse(sn,200,"application/json",m_respBuf,(uint16_t)n);
}

// ── GET /api/channels ─────────────────────────────────────────────────────────
void WebServer::handleApiChannels(uint8_t sn){
    const RuntimeConfig& cfg=Cfg();
    uint8_t st=m_app?m_app->getChannelStatus():0;

    bool e=cfg.channels.eth_enabled     ||cfg.eth_enabled;
    bool g=cfg.channels.gsm_enabled     ||cfg.gsm_enabled;
    bool w=cfg.channels.wifi_enabled    ||cfg.wifi_enabled;
    bool i=cfg.channels.iridium_enabled ||cfg.iridium_enabled;

    const char* se = e?((st&0x01)?"ACTIVE":"STANDBY"):"DISABLED";
    const char* sg = g?((st&0x02)?"ACTIVE":"STANDBY"):"DISABLED";
    const char* sw = w?((st&0x04)?"ACTIVE":"STANDBY"):"DISABLED";
    const char* si = i?((st&0x08)?"ACTIVE":"STANDBY"):"DISABLED";

    char buf[768];
    int len=std::snprintf(buf,sizeof(buf),
        "{\"channels\":["
        "{\"name\":\"Ethernet W5500\",\"status\":\"%s\",\"label\":\"%s\",\"priority\":\"1\"},"
        "{\"name\":\"GSM Air780E\",   \"status\":\"%s\",\"label\":\"%s\",\"priority\":\"2\"},"
        "{\"name\":\"WiFi ESP8266\",  \"status\":\"%s\",\"label\":\"%s\",\"priority\":\"%s\"},"
        "{\"name\":\"Iridium SBD\",   \"status\":\"%s\",\"label\":\"%s\",\"priority\":\"%s\"}"
        "],"
        "\"eth\":\"%s\",\"gsm\":\"%s\",\"wifi\":\"%s\",\"iridium\":\"%s\"}",
        se,se, sg,sg, sw,sw,w?"3":"—", si,si,i?"4":"—",
        se,sg,sw,si);
    sendResponse(sn,200,"application/json",buf,(uint16_t)len);
}

// ── GET /api/web_mode ─────────────────────────────────────────────────────────
void WebServer::handleApiWebMode(uint8_t sn){
    bool active=m_app?m_app->isWebActive():false;
    uint16_t rem=m_app?m_app->webIdleRemainingS():0;
    char buf[128];
    int len=std::snprintf(buf,sizeof(buf),
        "{\"web_active\":%s,\"remaining\":%u,\"idle_remaining_s\":%u}",
        active?"true":"false",(unsigned)rem,(unsigned)rem);
    sendResponse(sn,200,"application/json",buf,(uint16_t)len);
}

// ── POST /api/test_send  /  GET /api/test_result ──────────────────────────────
void WebServer::handleApiTestSend(uint8_t sn){
    if(m_app) m_app->triggerTestSend();
    const char* r="{\"ok\":true}";
    sendResponse(sn,200,"application/json",r,(uint16_t)std::strlen(r));
}
void WebServer::handleApiTestResult(uint8_t sn){
    char state[16]{},channel[16]{}; int httpCode=0; uint32_t elapsed=0;
    if(m_app) m_app->getTestResult(state,channel,&httpCode,&elapsed);
    else std::strncpy(state,"idle",sizeof(state)-1);
    char buf[256];
    int len=std::snprintf(buf,sizeof(buf),
        "{\"status\":\"%s\",\"channel\":\"%s\","
        "\"http_code\":%d,\"elapsed_ms\":%lu}",
        state,channel,httpCode,(unsigned long)elapsed);
    sendResponse(sn,200,"application/json",buf,(uint16_t)len);
}

// ── GET /api/logs ─────────────────────────────────────────────────────────────
void WebServer::handleApiLogs(uint8_t sn,const char* queryStr){
    uint32_t offset=0; uint16_t count=50;
    if(queryStr&&queryStr[0]){
        const char* p=std::strstr(queryStr,"offset=");
        if(p) offset=(uint32_t)std::atoi(p+7);
        p=std::strstr(queryStr,"count=");
        if(p){count=(uint16_t)std::atoi(p+6);if(count>100)count=100;}
    }
    CircularLogBuffer& log=CircularLogBuffer::instance();
    uint16_t total=log.getCount();
    char resp[RESP_BUF_SIZE]; int pos=0;
    pos+=std::snprintf(resp+pos,sizeof(resp)-pos,
        "{\"total\":%lu,\"lines\":[",(unsigned long)log.getTotal());
    bool first=true;
    char line[CircularLogBuffer::LINE_SIZE];
    for(uint16_t i=0;i<count&&(offset+i)<total;++i){
        if(!log.getLine((uint16_t)(offset+i),line,sizeof(line))) continue;
        if(!first&&pos<(int)sizeof(resp)-10) resp[pos++]=',';
        if(pos<(int)sizeof(resp)-(int)CircularLogBuffer::LINE_SIZE-4){
            resp[pos++]='"';
            for(char* c=line;*c&&pos<(int)sizeof(resp)-4;++c){
                if(*c=='"') resp[pos++]='\\';
                resp[pos++]=*c;
            }
            resp[pos++]='"'; first=false;
        }
    }
    if(pos<(int)sizeof(resp)-2){resp[pos++]=']';resp[pos++]='}';resp[pos]='\0';}
    sendResponse(sn,200,"application/json",resp,(uint16_t)pos);
}
void WebServer::handleApiLogsExport(uint8_t sn)
{
    CircularLogBuffer& log = CircularLogBuffer::instance();
    uint16_t total = log.getCount();

    // Отправляем заголовок вручную (размер заранее не известен)
    char hdr[256];
    int hl = std::snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=UTF-8\r\n"
        "Content-Disposition: attachment; filename=\"monset.log\"\r\n"
        "Access-Control-Allow-Origin: *\r\nTransfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n");
    send(sn, (uint8_t*)hdr, (uint16_t)hl);

    char line[CircularLogBuffer::LINE_SIZE];
    for (uint16_t i = 0; i < total; i++) {
        if (log.getLine(i, line, sizeof(line))) {
            // chunked HTTP: размер строки в hex
            char chunk_hdr[12];
            int chl = std::snprintf(chunk_hdr, sizeof(chunk_hdr),
                "%X\r\n", (unsigned)std::strlen(line));
            send(sn, (uint8_t*)chunk_hdr, (uint16_t)chl);
            send(sn, (uint8_t*)line, (uint16_t)std::strlen(line));
            send(sn, (uint8_t*)"\r\n", 2);
            IWDG->KR = 0xAAAA;
        }
    }
    // Завершающий chunk
    send(sn, (uint8_t*)"0\r\n\r\n", 5);
}
void WebServer::handleApiLogsClear(uint8_t sn){
    CircularLogBuffer::instance().clear();
    const char* r="{\"ok\":true}";
    sendResponse(sn,200,"application/json",r,(uint16_t)std::strlen(r));
}

// ── POST /config ──────────────────────────────────────────────────────────────
void WebServer::handlePostConfig(uint8_t sn,const char* body){
    if(!body||std::strlen(body)<2){
        const char* r="{\"error\":\"empty body\"}";
        sendResponse(sn,400,"application/json",r,(uint16_t)std::strlen(r)); return;
    }
    RuntimeConfig newCfg=Cfg();
    if(newCfg.loadFromJson(body,std::strlen(body))){
        Cfg()=newCfg;
        Cfg().saveToSd(RUNTIME_CONFIG_FILENAME);
        const char* r="{\"status\":\"ok\",\"message\":\"Saved. Reboot to apply.\"}";
        sendResponse(sn,200,"application/json",r,(uint16_t)std::strlen(r));
        DBG.info("WebServer: config updated via web");
    }else{
        const char* r="{\"error\":\"JSON parse failed\"}";
        sendResponse(sn,400,"application/json",r,(uint16_t)std::strlen(r));
    }
}

// ── Router ────────────────────────────────────────────────────────────────────
void WebServer::handleRequest(uint8_t sn,const char* request,uint16_t reqLen){
    if(!checkAuth(request)){send401(sn);return;}
    if(m_activityCb) m_activityCb(m_activityCtx);
    if(m_app) m_app->notifyWebActivity();

    char method[8]{},path[64]{};
    std::sscanf(request,"%7s %63s",method,path);
    DBG.info("WebServer: %s %s",method,path);

    char cleanPath[64]; std::strncpy(cleanPath,path,sizeof(cleanPath)-1);
    char* qs=std::strchr(cleanPath,'?'); if(qs)*qs='\0';
    const char* queryStr=qs?(std::strchr(path,'?')+1):"";

    if(std::strcmp(method,"GET")==0){
        if(std::strcmp(cleanPath,"/")==0||
           std::strcmp(cleanPath,"/config")==0||
           std::strcmp(cleanPath,"/config.html")==0)     handleConfig(sn);
        else if(std::strcmp(cleanPath,"/dashboard")==0||
                std::strcmp(cleanPath,"/index.html")==0) handleIndex(sn);
        else if(std::strcmp(cleanPath,"/logs")==0||
                std::strcmp(cleanPath,"/logs.html")==0)  handleLogs(sn);
        else if(std::strcmp(cleanPath,"/test")==0)        handleTestPage(sn);
        else if(std::strcmp(cleanPath,"/export")==0)      handleExport(sn);
        else if(std::strcmp(cleanPath,"/api/sensors")==0) handleApiSensors(sn);
        else if(std::strcmp(cleanPath,"/api/config")==0)  handleApiConfig(sn);
        else if(std::strcmp(cleanPath,"/api/channels")==0)handleApiChannels(sn);
        else if(std::strcmp(cleanPath,"/api/web_mode")==0)handleApiWebMode(sn);
        else if(std::strcmp(cleanPath,"/api/test_result")==0)handleApiTestResult(sn);
        else if(std::strcmp(cleanPath,"/api/logs/export")==0)handleApiLogsExport(sn);
        else if(std::strncmp(cleanPath,"/api/logs",9)==0) handleApiLogs(sn,queryStr);
        else{
            const char* ext=std::strrchr(cleanPath,'.');
            const char* ct="text/plain";
            if(ext){
                if(std::strcmp(ext,".html")==0) ct="text/html";
                else if(std::strcmp(ext,".css")==0) ct="text/css";
                else if(std::strcmp(ext,".js")==0)  ct="application/javascript";
                else if(std::strcmp(ext,".json")==0)ct="application/json";
            }
            if(!serveFile(sn,cleanPath,ct)) send404(sn);
        }
    }
    else if(std::strcmp(method,"POST")==0){
        const char* body=std::strstr(request,"\r\n\r\n");
        if(body) body+=4;
        if(std::strcmp(cleanPath,"/config")==0||
           std::strcmp(cleanPath,"/api/config")==0) handlePostConfig(sn,body);
        else if(std::strcmp(cleanPath,"/api/test_send")==0)  handleApiTestSend(sn);
        else if(std::strcmp(cleanPath,"/api/logs/clear")==0) handleApiLogsClear(sn);
        else send404(sn);
    }
    else send404(sn);
}

// ── tick ──────────────────────────────────────────────────────────────────────
void WebServer::tick(){
    if(!m_running) return;
    uint8_t status=getSn_SR(HTTP_SOCKET);
    switch(status){
        case SOCK_ESTABLISHED:{
            int32_t len=getSn_RX_RSR(HTTP_SOCKET);
            if(len>0){
                if(!g_web_exclusive){
                    g_web_exclusive=true;
                    DBG.info("WebServer: WEB EXCLUSIVE MODE ON");
                    closeDataSockets(HTTP_SOCKET);
                }
                if(len>REQ_BUF_SIZE-1) len=REQ_BUF_SIZE-1;
                int32_t rx=recv(HTTP_SOCKET,(uint8_t*)m_reqBuf,(uint16_t)len);
                if(rx>0){m_reqBuf[rx]=0;handleRequest(HTTP_SOCKET,m_reqBuf,(uint16_t)rx);}
                disconnect(HTTP_SOCKET);
            }
            break;
        }
        case SOCK_CLOSE_WAIT: disconnect(HTTP_SOCKET); break;
        case SOCK_CLOSED:
            if(socket(HTTP_SOCKET,Sn_MR_TCP,HTTP_PORT,0)==HTTP_SOCKET) listen(HTTP_SOCKET);
            break;
        case SOCK_LISTEN: break;
        default: break;
    }
}
