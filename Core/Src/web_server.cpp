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
    // Embedded fallback — /config.html not found on SD
    {
        char hdr[128];
        int hl=std::snprintf(hdr,sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n"
            "Content-Length: 29280\r\nAccess-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n");
        send(sn,(uint8_t*)hdr,(uint16_t)hl);
        {static const char p0[]="<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>MonSet \xe2\x80\x94 \xd0\x9a\xd0\xbe\xd0\xbd\xd1\x84\xd0\xb8\xd0\xb3\xd1\x83\xd1\x80\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f</title><style>*{box-sizing:border-box;margin:0;padding:0}body{background:#0e1117;color:#c9d1d9;font:14px/1.5 system-ui,sans-serif;padding-bottom:70px}a{color:#2dd4bf;text-decoration:none}.nav{background:#161b22;border-bottom:1px solid #30363d;display:flex;gap:4px;padding:8px 12px}.nav a{padding:6px 14px;border-radius:6px;color:#c9d1d9;font-s";
        send(sn,(uint8_t*)p0,(uint16_t)sizeof(p0)-1); IWDG->KR=0xAAAA;}
        {static const char p1[]="ize:13px}.nav a.act{background:#2dd4bf;color:#0e1117;font-weight:600}.hdr{padding:14px 16px;border-bottom:1px solid #30363d;background:#161b22;font-size:18px;font-weight:700;color:#2dd4bf}details{background:#161b22;border:1px solid #30363d;border-radius:8px;margin:10px}summary{padding:10px 14px;cursor:pointer;font-weight:600;font-size:13px;color:#2dd4bf;user-select:none;list-style:none;display:flex;align-items:center;gap:6px}summary::before{content:\"\xe2\x96\xb6\";font-size:10px;transition:transform .2s}details[open]";
        send(sn,(uint8_t*)p1,(uint16_t)sizeof(p1)-1); IWDG->KR=0xAAAA;}
        {static const char p2[]=">summary::before{transform:rotate(90deg)}.sc{padding:12px 14px 14px}.r{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:8px;align-items:center}.f{display:flex;flex-direction:column;gap:3px;flex:1;min-width:120px}label{font-size:11px;color:#8b949e}input[type=text],input[type=number],input[type=password],select{background:#0e1117;border:1px solid #30363d;border-radius:5px;color:#c9d1d9;padding:5px 8px;font-size:13px;width:100%}input:focus,select:focus{outline:none;border-color:#2dd4bf}.tg{display:flex;align-";
        send(sn,(uint8_t*)p2,(uint16_t)sizeof(p2)-1); IWDG->KR=0xAAAA;}
        {static const char p3[]="items:center;gap:8px}.tg input[type=checkbox]{appearance:none;width:36px;height:20px;background:#30363d;border-radius:10px;cursor:pointer;position:relative;flex-shrink:0;transition:background .2s}.tg input[type=checkbox]:checked{background:#2dd4bf}.tg input[type=checkbox]::after{content:\"\";position:absolute;width:14px;height:14px;background:#fff;border-radius:50%;top:3px;left:3px;transition:left .2s}.tg input[type=checkbox]:checked::after{left:19px}.tg span{font-size:13px;white-space:nowrap}.btn{background:";
        send(sn,(uint8_t*)p3,(uint16_t)sizeof(p3)-1); IWDG->KR=0xAAAA;}
        {static const char p4[]="#2dd4bf;color:#0e1117;border:none;border-radius:6px;padding:6px 14px;cursor:pointer;font-size:13px;font-weight:600}.btn:hover{opacity:.85}.btn.s{background:#30363d;color:#c9d1d9}.cd{background:#0e1117;border:1px solid #30363d;border-radius:6px;padding:10px;margin-bottom:8px}.ch{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}.tabs{display:flex;gap:4px;margin-bottom:10px}.tab{padding:5px 12px;border-radius:5px;cursor:pointer;font-size:12px;background:#0e1117;border:1px solid #";
        send(sn,(uint8_t*)p4,(uint16_t)sizeof(p4)-1); IWDG->KR=0xAAAA;}
        {static const char p5[]="30363d;color:#c9d1d9}.tab.act{background:#2dd4bf;color:#0e1117;border-color:#2dd4bf;font-weight:600}.tp{display:none}.tp.act{display:block}.bar{position:fixed;bottom:0;left:0;right:0;background:#161b22;border-top:1px solid #30363d;padding:10px 16px;display:flex;gap:8px;z-index:99}.dr{display:flex;align-items:center;gap:6px;background:#0e1117;border:1px solid #30363d;border-radius:5px;padding:5px 8px;margin-bottom:5px}.dr span{flex:1;font-size:12px}.dr button{background:none;border:none;color:#8b949e;cursor:";
        send(sn,(uint8_t*)p5,(uint16_t)sizeof(p5)-1); IWDG->KR=0xAAAA;}
        {static const char p6[]="pointer;font-size:14px;padding:0 2px}.dr button:hover{color:#2dd4bf}table{width:100%;border-collapse:collapse;font-size:12px}th{background:#0e1117;color:#8b949e;padding:5px 6px;text-align:left;border-bottom:1px solid #30363d}td{padding:4px 5px;border-bottom:1px solid #1c2128}td input,td select{padding:3px 5px;font-size:12px}.msg{display:none;padding:6px 12px;border-radius:5px;font-size:13px;font-weight:600}.ok{background:#1a3a2e;color:#2dd4bf}.er{background:#3a1a1a;color:#f87171}.sh{font-size:12px;color:#8b";
        send(sn,(uint8_t*)p6,(uint16_t)sizeof(p6)-1); IWDG->KR=0xAAAA;}
        {static const char p7[]="949e;margin:8px 0 4px;font-weight:600}</style></head><body>\n<div class=\"hdr\">MonSet \xe2\x80\x94 \xd0\x9a\xd0\xbe\xd0\xbd\xd1\x84\xd0\xb8\xd0\xb3\xd1\x83\xd1\x80\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f</div>\n<nav class=\"nav\"><a href=\"/config\" class=\"act\">\xd0\x9a\xd0\xbe\xd0\xbd\xd1\x84\xd0\xb8\xd0\xb3</a><a href=\"/\">\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82</a><a href=\"/logs\">\xd0\x9b\xd0\xbe\xd0\xb3</a></nav>\n<details open><summary>A: \xd0\x9a\xd0\xb0\xd0\xbd\xd0\xb0\xd0\xbb\xd1\x8b \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb4\xd0\xb0\xd1\x87\xd0\xb8</summary><div class=\"sc\">\n<div class=\"r\">\n<div class=\"tg\"><input type=\"checkbox\" id=\"ch_eth\" onchange=\"c('ch_eth',this.checked)\"><span>Ethernet W5500</span></div>\n<div class=\"tg\"><input type=\"checkbox\" id=\"ch_gsm\" onc";
        send(sn,(uint8_t*)p7,(uint16_t)sizeof(p7)-1); IWDG->KR=0xAAAA;}
        {static const char p8[]="hange=\"c('ch_gsm',this.checked)\"><span>GSM</span></div>\n<div class=\"tg\"><input type=\"checkbox\" id=\"ch_wifi\" onchange=\"c('ch_wifi',this.checked)\"><span>WiFi ESP8266</span></div>\n<div class=\"tg\"><input type=\"checkbox\" id=\"ch_iridium\" onchange=\"c('ch_iridium',this.checked)\"><span>Iridium SBD</span></div>\n<div class=\"tg\"><input type=\"checkbox\" id=\"chain_mode\" onchange=\"c('chain_mode',this.checked)\"><span>Chain mode</span></div>\n</div>\n<div style=\"font-size:11px;color:#8b949e;margin-bottom:4px\">\xd0\x9f\xd1\x80\xd0\xb8\xd0\xbe\xd1\x80\xd0\xb8\xd1\x82\xd0\xb5\xd1";
        send(sn,(uint8_t*)p8,(uint16_t)sizeof(p8)-1); IWDG->KR=0xAAAA;}
        {static const char p9[]="\x82:</div>\n<div id=\"cord\"></div>\n<div class=\"r\" style=\"margin-top:8px\">\n<div class=\"f\"><label>\xd0\xa2\xd0\xb0\xd0\xb9\xd0\xbc\xd0\xb0\xd1\x83\xd1\x82 (\xd1\x81)</label><input type=\"number\" id=\"channel_timeout_s\" value=\"30\" onchange=\"c('channel_timeout_s',+this.value)\"></div>\n<div class=\"f\"><label>\xd0\x9f\xd0\xbe\xd0\xb2\xd1\x82\xd0\xbe\xd1\x80 (\xd1\x81)</label><input type=\"number\" id=\"channel_retry_s\" value=\"10\" onchange=\"c('channel_retry_s',+this.value)\"></div>\n<div class=\"f\"><label>\xd0\x9c\xd0\xb0\xd0\xba\xd1\x81 \xd0\xbf\xd0\xbe\xd0\xbf\xd1\x8b\xd1\x82\xd0\xbe\xd0\xba</label><input type=\"number\" id=\"channel_max_retries\" value=\"3\" onchange=\"c('channel_max_";
        send(sn,(uint8_t*)p9,(uint16_t)sizeof(p9)-1); IWDG->KR=0xAAAA;}
        {static const char p10[]="retries',+this.value)\"></div>\n</div></div></details>\n<details><summary>B: \xd0\x9f\xd1\x80\xd0\xbe\xd1\x82\xd0\xbe\xd0\xba\xd0\xbe\xd0\xbb \xd0\xb8 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80</summary><div class=\"sc\">\n<div class=\"f\" style=\"max-width:260px;margin-bottom:10px\"><label>\xd0\x9f\xd1\x80\xd0\xbe\xd1\x82\xd0\xbe\xd0\xba\xd0\xbe\xd0\xbb</label>\n<select id=\"proto\" onchange=\"sp(this.value)\"><option value=\"https_tb\">HTTPS ThingsBoard</option><option value=\"mqtt_gen\">MQTT (Generic)</option><option value=\"mqtt_tb\">MQTT ThingsBoard</option><option value=\"webhook\">HTTP Webhook</option></select></div>\n<div id=\"g_tb\"><div class=\"r\"><div ";
        send(sn,(uint8_t*)p10,(uint16_t)sizeof(p10)-1); IWDG->KR=0xAAAA;}
        {static const char p11[]="class=\"f\"><label>\xd0\xa5\xd0\xbe\xd1\x81\xd1\x82 TB</label><input type=\"text\" id=\"tb_host\" onchange=\"c('tb_host',this.value)\"></div><div class=\"f\"><label>\xd0\xa2\xd0\xbe\xd0\xba\xd0\xb5\xd0\xbd</label><input type=\"text\" id=\"tb_token\" onchange=\"c('tb_token',this.value)\"></div><div class=\"f\" style=\"max-width:90px\"><label>\xd0\x9f\xd0\xbe\xd1\x80\xd1\x82</label><input type=\"number\" id=\"tb_port\" value=\"443\" onchange=\"c('tb_port',+this.value)\"></div></div></div>\n<div id=\"g_mqtt\" style=\"display:none\"><div class=\"r\"><div class=\"f\"><label>MQTT \xd0\xa5\xd0\xbe\xd1\x81\xd1\x82</label><input type=\"text\" id=\"mqtt_ho";
        send(sn,(uint8_t*)p11,(uint16_t)sizeof(p11)-1); IWDG->KR=0xAAAA;}
        {static const char p12[]="st\" onchange=\"c('mqtt_host',this.value)\"></div><div class=\"f\" style=\"max-width:90px\"><label>\xd0\x9f\xd0\xbe\xd1\x80\xd1\x82</label><input type=\"number\" id=\"mqtt_port\" value=\"1883\" onchange=\"c('mqtt_port',+this.value)\"></div></div>\n<div class=\"r\"><div class=\"f\"><label>\xd0\x9f\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xb7\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c</label><input type=\"text\" id=\"mqtt_user\" onchange=\"c('mqtt_user',this.value)\"></div><div class=\"f\"><label>\xd0\x9f\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c</label><input type=\"password\" id=\"mqtt_pass\" onchange=\"c('mqtt_pass',this.value)\"></div></div>\n<div class=\"r\"><div class=\"f";
        send(sn,(uint8_t*)p12,(uint16_t)sizeof(p12)-1); IWDG->KR=0xAAAA;}
        {static const char p13[]="\"><label>\xd0\xa2\xd0\xbe\xd0\xbf\xd0\xb8\xd0\xba</label><input type=\"text\" id=\"mqtt_topic\" onchange=\"c('mqtt_topic',this.value)\"></div><div class=\"f\" style=\"max-width:80px\"><label>QoS</label><select id=\"mqtt_qos\" onchange=\"c('mqtt_qos',+this.value)\"><option>0</option><option>1</option><option>2</option></select></div><div class=\"tg\" style=\"margin-top:16px\"><input type=\"checkbox\" id=\"mqtt_tls\" onchange=\"c('mqtt_tls',this.checked)\"><span>TLS</span></div></div></div>\n<div id=\"g_wh\" style=\"display:none\"><div class=\"r\"><div class=\"f\"><label";
        send(sn,(uint8_t*)p13,(uint16_t)sizeof(p13)-1); IWDG->KR=0xAAAA;}
        {static const char p14[]=">URL Webhook</label><input type=\"text\" id=\"webhook_url\" onchange=\"c('webhook_url',this.value)\"></div><div class=\"f\" style=\"max-width:100px\"><label>\xd0\x9c\xd0\xb5\xd1\x82\xd0\xbe\xd0\xb4</label><select id=\"webhook_method\" onchange=\"c('webhook_method',this.value)\"><option>GET</option><option selected>POST</option><option>PUT</option></select></div></div></div>\n</div></details>\n<details><summary>C: \xd0\xa0\xd0\xb0\xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5 \xd0\xb8 \xd0\xbe\xd0\xbf\xd1\x80\xd0\xbe\xd1\x81</summary><div class=\"sc\">\n<div class=\"r\">\n<div class=\"f\"><label>\xd0\x9e\xd0\xbf\xd1\x80\xd0\xbe\xd1\x81 (\xd1\x81)</label><input type=\"number";
        send(sn,(uint8_t*)p14,(uint16_t)sizeof(p14)-1); IWDG->KR=0xAAAA;}
        {static const char p15[]="\" id=\"poll_interval_s\" value=\"10\" onchange=\"c('poll_interval_s',+this.value)\"></div>\n<div class=\"f\"><label>\xd0\x9e\xd1\x82\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xba\xd0\xb0 (\xd1\x81)</label><input type=\"number\" id=\"send_interval_s\" value=\"60\" onchange=\"c('send_interval_s',+this.value)\"></div>\n<div class=\"f\"><label>\xd0\xa0\xd0\xb5\xd0\xb7\xd0\xb5\xd1\x80\xd0\xb2 \xd0\xbf\xd0\xbe\xd0\xb2\xd1\x82\xd0\xbe\xd1\x80 (\xd1\x81)</label><input type=\"number\" id=\"backup_retry_s\" value=\"300\" onchange=\"c('backup_retry_s',+this.value)\"></div>\n<div class=\"f\" style=\"max-width:90px\"><label>\xd0\xa3\xd1\x81\xd1\x80\xd0\xb5\xd0\xb4\xd0\xbd\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5</label><input type=\"number\" id=\"avg_count\" ";
        send(sn,(uint8_t*)p15,(uint16_t)sizeof(p15)-1); IWDG->KR=0xAAAA;}
        {static const char p16[]="value=\"1\" onchange=\"c('avg_count',+this.value)\"></div>\n</div>\n<div class=\"r\"><div class=\"tg\"><input type=\"checkbox\" id=\"deep_sleep_enabled\" onchange=\"c('deep_sleep_enabled',this.checked);sh('ds_f',this.checked)\"><span>Deep sleep</span></div><div class=\"f\" id=\"ds_f\" style=\"max-width:130px;display:none\"><label>\xd0\x94\xd0\xbb\xd0\xb8\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c (\xd1\x81)</label><input type=\"number\" id=\"deep_sleep_s\" value=\"3600\" onchange=\"c('deep_sleep_s',+this.value)\"></div></div>\n<div class=\"r\"><div class=\"tg\"><input type=\"checkbox\" id=\"s";
        send(sn,(uint8_t*)p16,(uint16_t)sizeof(p16)-1); IWDG->KR=0xAAAA;}
        {static const char p17[]="chedule_enabled\" onchange=\"c('schedule_enabled',this.checked);sh('sch_f',this.checked)\"><span>\xd0\xa0\xd0\xb0\xd1\x81\xd0\xbf\xd0\xb8\xd1\x81\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5</span></div><div id=\"sch_f\" style=\"display:none;display:flex;gap:8px\"><div class=\"f\" style=\"max-width:90px\"><label>\xd0\xa1\xd1\x82\xd0\xb0\xd1\x80\xd1\x82</label><input type=\"text\" id=\"schedule_start\" placeholder=\"06:00\" onchange=\"c('schedule_start',this.value)\"></div><div class=\"f\" style=\"max-width:90px\"><label>\xd0\xa1\xd1\x82\xd0\xbe\xd0\xbf</label><input type=\"text\" id=\"schedule_stop\" placeholder=\"22:00\" onchange=\"c('schedule_stop',this.value";
        send(sn,(uint8_t*)p17,(uint16_t)sizeof(p17)-1); IWDG->KR=0xAAAA;}
        {static const char p18[]=")\"></div></div></div>\n</div></details>\n<details><summary>D: \xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba\xd0\xb8 Modbus RTU</summary><div class=\"sc\">\n<div class=\"tabs\"><div class=\"tab act\" onclick=\"st(this,'tp0')\">USART3</div><div class=\"tab\" onclick=\"st(this,'tp1')\">UART4</div><div class=\"tab\" onclick=\"st(this,'tp2')\">UART5</div></div>\n<div id=\"tp0\" class=\"tp act\"></div><div id=\"tp1\" class=\"tp\"></div><div id=\"tp2\" class=\"tp\"></div>\n</div></details>\n<details><summary>E: Modbus TCP Master</summary><div class=\"sc\">\n<div class=\"tg\" style=\"margin-b";
        send(sn,(uint8_t*)p18,(uint16_t)sizeof(p18)-1); IWDG->KR=0xAAAA;}
        {static const char p19[]="ottom:10px\"><input type=\"checkbox\" id=\"mtcpm_en\" onchange=\"c('mtcpm_en',this.checked)\"><span>\xd0\x92\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c</span></div>\n<div id=\"tcp_devs\"></div><button class=\"btn s\" onclick=\"addTD()\">+ \xd0\x94\xd0\xbe\xd0\xb1\xd0\xb0\xd0\xb2\xd0\xb8\xd1\x82\xd1\x8c \xd1\x83\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd1\x81\xd1\x82\xd0\xb2\xd0\xbe TCP</button>\n</div></details>\n<details><summary>F: Modbus TCP Slave</summary><div class=\"sc\">\n<div class=\"tg\" style=\"margin-bottom:10px\"><input type=\"checkbox\" id=\"mtcps_en\" onchange=\"c('mtcps_en',this.checked)\"><span>\xd0\x92\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c</span></div>\n<div class=\"r\"><div class=\"f\" style=";
        send(sn,(uint8_t*)p19,(uint16_t)sizeof(p19)-1); IWDG->KR=0xAAAA;}
        {static const char p20[]="\"max-width:90px\"><label>\xd0\x9f\xd0\xbe\xd1\x80\xd1\x82</label><input type=\"number\" id=\"sl_port\" value=\"502\" onchange=\"c('sl_port',+this.value)\"></div><div class=\"f\" style=\"max-width:90px\"><label>Unit ID</label><input type=\"number\" id=\"sl_uid\" value=\"1\" onchange=\"c('sl_uid',+this.value)\"></div><div class=\"f\" style=\"max-width:90px\"><label>W5500 \xd1\x81\xd0\xbe\xd0\xba\xd0\xb5\xd1\x82</label><input type=\"number\" id=\"sl_sock\" value=\"0\" onchange=\"c('sl_sock',+this.value)\"></div><div class=\"f\" style=\"max-width:140px\"><label>\xd0\xa2\xd0\xb0\xd0\xb9\xd0\xbc\xd0\xb0\xd1\x83\xd1\x82 \xd1\x81\xd0\xbe\xd0\xb5\xd0\xb4. (\xd0\xbc\xd1\x81)</label>";
        send(sn,(uint8_t*)p20,(uint16_t)sizeof(p20)-1); IWDG->KR=0xAAAA;}
        {static const char p21[]="<input type=\"number\" id=\"sl_ctms\" value=\"5000\" onchange=\"c('sl_ctms',+this.value)\"></div></div>\n<table><thead><tr><th>\xd0\x90\xd0\xb4\xd1\x80\xd0\xb5\xd1\x81</th><th>\xd0\x98\xd1\x81\xd1\x82\xd0\xbe\xd1\x87\xd0\xbd\xd0\xb8\xd0\xba</th><th>\xd0\xa2\xd0\xb8\xd0\xbf</th><th>\xd0\x9c\xd0\xb0\xd1\x81\xd1\x88\xd1\x82\xd0\xb0\xd0\xb1</th><th>\xd0\x95\xd0\xb4.</th><th>\xd0\x98\xd0\xbc\xd1\x8f</th><th></th></tr></thead><tbody id=\"regtbl\"></tbody></table>\n<button class=\"btn s\" style=\"margin-top:6px\" onclick=\"addReg()\">+ \xd0\x94\xd0\xbe\xd0\xb1\xd0\xb0\xd0\xb2\xd0\xb8\xd1\x82\xd1\x8c \xd1\x80\xd0\xb5\xd0\xb3\xd0\xb8\xd1\x81\xd1\x82\xd1\x80</button>\n</div></details>\n<details><summary>G: \xd0\xa1\xd0\xb5\xd1\x82\xd1\x8c</summary><div class=\"sc\">\n<div class=\"sh\">Ethernet</div>\n<div class=\"r\"><di";
        send(sn,(uint8_t*)p21,(uint16_t)sizeof(p21)-1); IWDG->KR=0xAAAA;}
        {static const char p22[]="v class=\"tg\"><input type=\"checkbox\" id=\"eth_dhcp\" checked onchange=\"c('eth_dhcp',this.checked);sh('eth_st',!this.checked)\"><span>DHCP</span></div></div>\n<div id=\"eth_st\" style=\"display:none\"><div class=\"r\"><div class=\"f\"><label>IP</label><input type=\"text\" id=\"eth_ip\" onchange=\"c('eth_ip',this.value)\"></div><div class=\"f\"><label>\xd0\x9c\xd0\xb0\xd1\x81\xd0\xba\xd0\xb0</label><input type=\"text\" id=\"eth_sn\" onchange=\"c('eth_sn',this.value)\"></div><div class=\"f\"><label>\xd0\xa8\xd0\xbb\xd1\x8e\xd0\xb7</label><input type=\"text\" id=\"eth_gw\" onchange=\"c('eth_gw',th";
        send(sn,(uint8_t*)p22,(uint16_t)sizeof(p22)-1); IWDG->KR=0xAAAA;}
        {static const char p23[]="is.value)\"></div><div class=\"f\"><label>DNS</label><input type=\"text\" id=\"eth_dns\" onchange=\"c('eth_dns',this.value)\"></div></div></div>\n<div class=\"sh\">GSM</div>\n<div class=\"r\"><div class=\"f\"><label>APN</label><input type=\"text\" id=\"gsm_apn\" onchange=\"c('gsm_apn',this.value)\"></div><div class=\"f\"><label>\xd0\x9f\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xb7\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c</label><input type=\"text\" id=\"gsm_user\" onchange=\"c('gsm_user',this.value)\"></div><div class=\"f\"><label>\xd0\x9f\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c</label><input type=\"password\" id=\"gsm_pass\" onchange=\"c('gsm_pass";
        send(sn,(uint8_t*)p23,(uint16_t)sizeof(p23)-1); IWDG->KR=0xAAAA;}
        {static const char p24[]="',this.value)\"></div></div>\n<div class=\"sh\">WiFi</div>\n<div class=\"r\"><div class=\"f\"><label>SSID</label><input type=\"text\" id=\"wifi_ssid\" onchange=\"c('wifi_ssid',this.value)\"></div><div class=\"f\"><label>\xd0\x9f\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c</label><input type=\"password\" id=\"wifi_pass\" onchange=\"c('wifi_pass',this.value)\"></div></div>\n</div></details>\n<details><summary>H: \xd0\x92\xd1\x80\xd0\xb5\xd0\xbc\xd1\x8f \xd0\xb8 NTP</summary><div class=\"sc\">\n<div class=\"r\"><div class=\"tg\"><input type=\"checkbox\" id=\"ntp_enabled\" onchange=\"c('ntp_enabled',this.checked)\"><span>N";
        send(sn,(uint8_t*)p24,(uint16_t)sizeof(p24)-1); IWDG->KR=0xAAAA;}
        {static const char p25[]="TP</span></div><div class=\"f\"><label>\xd0\xa1\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80</label><input type=\"text\" id=\"ntp_server\" value=\"pool.ntp.org\" onchange=\"c('ntp_server',this.value)\"></div><div class=\"f\" style=\"max-width:130px\"><label>\xd0\xa7\xd0\xb0\xd1\x81\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb9 \xd0\xbf\xd0\xbe\xd1\x8f\xd1\x81</label><select id=\"tz_off\" onchange=\"c('tz_off',+this.value)\"></select></div></div>\n</div></details>\n<details><summary>I: \xd0\x92\xd0\xb5\xd0\xb1 \xd0\xb8 \xd0\xb1\xd0\xb5\xd0\xb7\xd0\xbe\xd0\xbf\xd0\xb0\xd1\x81\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c</summary><div class=\"sc\">\n<div class=\"r\"><div class=\"f\"><label>\xd0\x9f\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xb7\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c</label><input type=\"text\" id=\"web_user\" ";
        send(sn,(uint8_t*)p25,(uint16_t)sizeof(p25)-1); IWDG->KR=0xAAAA;}
        {static const char p26[]="value=\"admin\" onchange=\"c('web_user',this.value)\"></div><div class=\"f\"><label>\xd0\x9f\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c</label><input type=\"password\" id=\"web_pass\" onchange=\"c('web_pass',this.value)\"></div><div class=\"f\" style=\"max-width:80px\"><label>\xd0\x9f\xd0\xbe\xd1\x80\xd1\x82</label><input type=\"number\" id=\"web_port\" value=\"80\" onchange=\"c('web_port',+this.value)\"></div></div>\n<div class=\"r\"><div class=\"tg\"><input type=\"checkbox\" id=\"web_auth_enabled\" onchange=\"c('web_auth_enabled',this.checked)\"><span>\xd0\x90\xd1\x83\xd1\x82\xd0\xb5\xd0\xbd\xd1\x82\xd0\xb8\xd1\x84\xd0\xb8\xd0\xba\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f</span></div><div clas";
        send(sn,(uint8_t*)p26,(uint16_t)sizeof(p26)-1); IWDG->KR=0xAAAA;}
        {static const char p27[]="s=\"tg\"><input type=\"checkbox\" id=\"web_exclusive_mode\" onchange=\"c('web_exclusive_mode',this.checked)\"><span>\xd0\xad\xd0\xba\xd1\x81\xd0\xba\xd0\xbb\xd1\x8e\xd0\xb7\xd0\xb8\xd0\xb2\xd0\xbd\xd1\x8b\xd0\xb9 \xd1\x80\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc</span></div><div class=\"f\" style=\"max-width:150px\"><label>\xd0\xa2\xd0\xb0\xd0\xb9\xd0\xbc\xd0\xb0\xd1\x83\xd1\x82 \xd0\xbf\xd1\x80\xd0\xbe\xd1\x81\xd1\x82\xd0\xbe\xd1\x8f (\xd1\x81)</label><input type=\"number\" id=\"web_idle_timeout_s\" value=\"300\" onchange=\"c('web_idle_timeout_s',+this.value)\"></div></div>\n</div></details>\n<details><summary>J: \xd0\x9e\xd0\xbf\xd0\xbe\xd0\xb2\xd0\xb5\xd1\x89\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f</summary><div class=\"sc\">\n<div class=\"r\"><div class=\"tg\"><input type=\"checkbox\" id=\"alerts_e";
        send(sn,(uint8_t*)p27,(uint16_t)sizeof(p27)-1); IWDG->KR=0xAAAA;}
        {static const char p28[]="nabled\" onchange=\"c('alerts_enabled',this.checked)\"><span>\xd0\x9e\xd0\xbf\xd0\xbe\xd0\xb2\xd0\xb5\xd1\x89\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f</span></div><div class=\"f\" style=\"max-width:150px\"><label>\xd0\x91\xd0\xb0\xd1\x82\xd0\xb0\xd1\x80\xd0\xb5\xd1\x8f \xd0\xbc\xd0\xb8\xd0\xbd. (%)</label><input type=\"number\" id=\"batt_low\" value=\"20\" onchange=\"c('battery_low_threshold_pct',+this.value)\"></div></div>\n<div style=\"display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:8px\"><div id=\"sns_min\"></div><div id=\"sns_max\"></div></div>\n<div class=\"r\"><div class=\"tg\"><input type=\"checkbox\" id=\"alert_ch_fail\" onchange=\"c('alert";
        send(sn,(uint8_t*)p28,(uint16_t)sizeof(p28)-1); IWDG->KR=0xAAAA;}
        {static const char p29[]="_on_channel_fail',this.checked)\"><span>\xd0\xa1\xd0\xb1\xd0\xbe\xd0\xb9 \xd0\xba\xd0\xb0\xd0\xbd\xd0\xb0\xd0\xbb\xd0\xb0</span></div><div class=\"tg\"><input type=\"checkbox\" id=\"alert_sn_fail\" onchange=\"c('alert_on_sensor_fail',this.checked)\"><span>\xd0\xa1\xd0\xb1\xd0\xbe\xd0\xb9 \xd0\xb4\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba\xd0\xb0</span></div></div>\n<div class=\"f\" style=\"margin-top:6px\"><label>Webhook \xd0\xbe\xd0\xbf\xd0\xbe\xd0\xb2\xd0\xb5\xd1\x89\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb9</label><input type=\"text\" id=\"alert_webhook_url\" onchange=\"c('alert_webhook_url',this.value)\"></div>\n</div></details>\n<div class=\"bar\"><button class=\"btn\" onclick=\"save()\">\xf0\x9f\x92\xbe \xd0\xa1\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x82\xd1\x8c</button><but";
        send(sn,(uint8_t*)p29,(uint16_t)sizeof(p29)-1); IWDG->KR=0xAAAA;}
        {static const char p30[]="ton class=\"btn s\" onclick=\"rst()\">\xe2\x86\xba \xd0\xa1\xd0\xb1\xd1\x80\xd0\xbe\xd1\x81</button><button class=\"btn s\" onclick=\"exp()\">\xe2\xac\x87 \xd0\xad\xd0\xba\xd1\x81\xd0\xbf\xd0\xbe\xd1\x80\xd1\x82 JSON</button><span class=\"msg\" id=\"msg\"></span></div>\n<script>\nvar cfg={};\nvar DT=['INT16','UINT16','INT32_BE','UINT32_BE','FLOAT32_BE'];\nvar SRC=['\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba 0','\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba 1','\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba 2','\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba 3','\xd0\x91\xd0\xb0\xd1\x82\xd0\xb0\xd1\x80\xd0\xb5\xd1\x8f%','\xd0\x91\xd0\xb0\xd1\x82\xd0\xb0\xd1\x80\xd0\xb5\xd1\x8f V','\xd0\xa1\xd1\x82\xd0\xb0\xd1\x82\xd1\x83\xd1\x81 \xd0\xba\xd0\xb0\xd0\xbd\xd0\xb0\xd0\xbb\xd0\xbe\xd0\xb2'];\nvar CHN=['Ethernet W5500','GSM','WiFi ESP8266','Iridium SBD'];\nvar cord=[0,1,2,3],coen=[false,false,false,false];\n";
        send(sn,(uint8_t*)p30,(uint16_t)sizeof(p30)-1); IWDG->KR=0xAAAA;}
        {static const char p31[]="\nfunction c(k,v){cfg[k]=v}\nfunction sh(id,v){document.getElementById(id).style.display=v?'flex':'none'}\n\nfunction sp(v){\n  document.getElementById('g_tb').style.display=(v==='https_tb'||v==='mqtt_tb')?'block':'none';\n  document.getElementById('g_mqtt').style.display=(v==='mqtt_gen'||v==='mqtt_tb')?'block':'none';\n  document.getElementById('g_wh').style.display=(v==='webhook')?'block':'none';\n  cfg.proto=v;\n}\n\nfunction st(el,id){\n  document.querySelectorAll('.tab').forEach(function(t){t.classList.remove('act";
        send(sn,(uint8_t*)p31,(uint16_t)sizeof(p31)-1); IWDG->KR=0xAAAA;}
        {static const char p32[]="')});\n  document.querySelectorAll('.tp').forEach(function(t){t.classList.remove('act')});\n  el.classList.add('act');document.getElementById(id).classList.add('act');\n}\n\nfunction mv(i,d){\n  var j=i+d;if(j<0||j>3)return;\n  var t=cord[i];cord[i]=cord[j];cord[j]=t;\n  var t2=coen[i];coen[i]=coen[j];coen[j]=t2;\n  rndOrd();\n}\n\nfunction rndOrd(){\n  var h='';\n  for(var i=0;i<4;i++){\n    h+='<div class=\"dr\"><input type=\"checkbox\" style=\"width:auto\"'+(coen[i]?' checked':'')\n      +' onchange=\"coen['+i+']=this.checked\"";
        send(sn,(uint8_t*)p32,(uint16_t)sizeof(p32)-1); IWDG->KR=0xAAAA;}
        {static const char p33[]="><span>'+CHN[cord[i]]+'</span>'\n      +'<button onclick=\"mv('+i+',-1)\">\xe2\x96\xb2</button><button onclick=\"mv('+i+',1)\">\xe2\x96\xbc</button></div>';\n  }\n  document.getElementById('cord').innerHTML=h;\n}\n\nvar BD=['2400','4800','9600','19200','38400','115200'];\nvar rtu=[];\n\nfunction rtuH(p){\n  var x=rtu[p];\n  var h='<div class=\"r\"><div class=\"tg\"><input type=\"checkbox\"'+(x.en?' checked':'')+' onchange=\"rtu['+p+'].en=this.checked\"><span>\xd0\x92\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c</span></div>'\n    +'<div class=\"f\" style=\"max-width:100px\"><label>\xd0\x91\xd0\xbe\xd0\xb4\xd1";
        send(sn,(uint8_t*)p33,(uint16_t)sizeof(p33)-1); IWDG->KR=0xAAAA;}
        {static const char p34[]="\x80\xd0\xb5\xd0\xb9\xd1\x82</label><select onchange=\"rtu['+p+'].baud=+this.value\">';\n  BD.forEach(function(b){h+='<option'+(x.baud==b?' selected':'')+'>'+b+'</option>'});\n  h+='</select></div><div class=\"f\" style=\"max-width:80px\"><label>\xd0\xa1\xd1\x82\xd0\xbe\xd0\xbf-\xd0\xb1\xd0\xb8\xd1\x82\xd1\x8b</label><select onchange=\"rtu['+p+'].sb=+this.value\"><option'+(x.sb==1?' selected':'')+'>1</option><option'+(x.sb==2?' selected':'')+'>2</option></select></div>'\n    +'<div class=\"f\" style=\"max-width:100px\"><label>\xd0\xa7\xd1\x91\xd1\x82\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c</label><select onchange=\"rtu['+p+'].par=this.va";
        send(sn,(uint8_t*)p34,(uint16_t)sizeof(p34)-1); IWDG->KR=0xAAAA;}
        {static const char p35[]="lue\"><option'+(x.par==\"None\"?' selected':'')+'>None</option><option'+(x.par==\"Even\"?' selected':'')+'>Even</option><option'+(x.par==\"Odd\"?' selected':'')+'>Odd</option></select></div>'\n    +'<div class=\"f\" style=\"max-width:120px\"><label>\xd0\xa2\xd0\xb0\xd0\xb9\xd0\xbc\xd0\xb0\xd1\x83\xd1\x82 \xd0\xbe\xd1\x82\xd0\xb2.(\xd0\xbc\xd1\x81)</label><input type=\"number\" value=\"'+x.rms+'\" onchange=\"rtu['+p+'].rms=+this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:120px\"><label>\xd0\x9c\xd0\xb5\xd0\xb6\xd1\x84\xd1\x80\xd0\xb5\xd0\xb9\xd0\xbc\xd0\xbe\xd0\xb2\xd1\x8b\xd0\xb9(\xd0\xbc\xd1\x81)</label><input type=\"number\" value=\"'+x.fms+'\" onchange=\"rtu['+p+'].fms=";
        send(sn,(uint8_t*)p35,(uint16_t)sizeof(p35)-1); IWDG->KR=0xAAAA;}
        {static const char p36[]="+this.value\"></div>'\n    +'</div><div id=\"rd'+p+'\"></div>'\n    +'<button class=\"btn s\" onclick=\"addRD('+p+')\">+ \xd0\x94\xd0\xbe\xd0\xb1\xd0\xb0\xd0\xb2\xd0\xb8\xd1\x82\xd1\x8c \xd1\x83\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd1\x81\xd1\x82\xd0\xb2\xd0\xbe</button>';\n  return h;\n}\n\nfunction rtuDH(p,i){\n  var d=rtu[p].devs[i];\n  return '<div class=\"cd\"><div class=\"ch\"><span style=\"font-size:12px;font-weight:600\">\xd0\xa3\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd1\x81\xd1\x82\xd0\xb2\xd0\xbe '+(i+1)+'</span>'\n    +'<button class=\"btn s\" style=\"padding:2px 8px;font-size:11px\" onclick=\"delRD('+p+','+i+')\">\xe2\x9c\x95</button></div>'\n    +'<div class=\"r\"><div class=\"tg\"><input type=";
        send(sn,(uint8_t*)p36,(uint16_t)sizeof(p36)-1); IWDG->KR=0xAAAA;}
        {static const char p37[]="\"checkbox\"'+(d.en?' checked':'')+' onchange=\"rtu['+p+'].devs['+i+'].en=this.checked\"><span>\xd0\x92\xd0\xba\xd0\xbb</span></div>'\n    +'<div class=\"f\" style=\"max-width:80px\"><label>Slave addr</label><input type=\"number\" value=\"'+d.sa+'\" onchange=\"rtu['+p+'].devs['+i+'].sa=+this.value\"></div>'\n    +'<div class=\"f\"><label>\xd0\x98\xd0\xbc\xd1\x8f</label><input type=\"text\" value=\"'+d.nm+'\" onchange=\"rtu['+p+'].devs['+i+'].nm=this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:80px\"><label>Func</label><select onchange=\"rtu['+p+'].devs['+i+'";
        send(sn,(uint8_t*)p37,(uint16_t)sizeof(p37)-1); IWDG->KR=0xAAAA;}
        {static const char p38[]="].fc=+this.value\"><option'+(d.fc==3?' selected':'')+' value=\"3\">03</option><option'+(d.fc==4?' selected':'')+' value=\"4\">04</option></select></div>'\n    +'<div class=\"f\" style=\"max-width:100px\"><label>Reg start</label><input type=\"text\" value=\"0x'+d.rs.toString(16).toUpperCase()+'\" onchange=\"rtu['+p+'].devs['+i+'].rs=parseInt(this.value)\"></div>'\n    +'<div class=\"f\" style=\"max-width:80px\"><label>Count</label><input type=\"number\" value=\"'+d.rc+'\" onchange=\"rtu['+p+'].devs['+i+'].rc=+this.value\"></div>'\n    ";
        send(sn,(uint8_t*)p38,(uint16_t)sizeof(p38)-1); IWDG->KR=0xAAAA;}
        {static const char p39[]="+'</div><div class=\"r\"><div class=\"f\" style=\"max-width:140px\"><label>Data type</label><select onchange=\"rtu['+p+'].devs['+i+'].dt=this.value\">'\n    +DT.map(function(t){return'<option'+(d.dt===t?' selected':'')+'>'+t+'</option>'}).join('')\n    +'</select></div><div class=\"f\" style=\"max-width:80px\"><label>\xd0\x9c\xd0\xb0\xd1\x81\xd1\x88\xd1\x82\xd0\xb0\xd0\xb1</label><input type=\"number\" step=\"any\" value=\"'+d.sc+'\" onchange=\"rtu['+p+'].devs['+i+'].sc=+this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:80px\"><label>\xd0\xa1\xd0\xbc\xd0\xb5\xd1\x89\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5</label><inp";
        send(sn,(uint8_t*)p39,(uint16_t)sizeof(p39)-1); IWDG->KR=0xAAAA;}
        {static const char p40[]="ut type=\"number\" step=\"any\" value=\"'+d.of+'\" onchange=\"rtu['+p+'].devs['+i+'].of=+this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:70px\"><label>\xd0\x95\xd0\xb4.</label><input type=\"text\" value=\"'+d.un+'\" onchange=\"rtu['+p+'].devs['+i+'].un=this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:80px\"><label>CH idx</label><input type=\"number\" value=\"'+d.ci+'\" onchange=\"rtu['+p+'].devs['+i+'].ci=+this.value\"></div>'\n    +'</div></div>';\n}\n\nfunction addRD(p){if(rtu[p].devs.length<8){rtu[p].devs.push({en:false,";
        send(sn,(uint8_t*)p40,(uint16_t)sizeof(p40)-1); IWDG->KR=0xAAAA;}
        {static const char p41[]="sa:1,nm:'',fc:3,rs:0,rc:1,dt:'INT16',sc:1,of:0,un:'',ci:0});rndRD(p)}}\nfunction delRD(p,i){rtu[p].devs.splice(i,1);rndRD(p)}\nfunction rndRD(p){var h='';rtu[p].devs.forEach(function(_,i){h+=rtuDH(p,i)});document.getElementById('rd'+p).innerHTML=h}\n\nvar tcpd=[];\nfunction tcpDH(i){\n  var d=tcpd[i];\n  return '<div class=\"cd\"><div class=\"ch\"><span style=\"font-size:12px;font-weight:600\">TCP '+(i+1)+'</span>'\n    +'<button class=\"btn s\" style=\"padding:2px 8px;font-size:11px\" onclick=\"delTD('+i+')\">\xe2\x9c\x95</button></di";
        send(sn,(uint8_t*)p41,(uint16_t)sizeof(p41)-1); IWDG->KR=0xAAAA;}
        {static const char p42[]="v>'\n    +'<div class=\"r\"><div class=\"tg\"><input type=\"checkbox\"'+(d.en?' checked':'')+' onchange=\"tcpd['+i+'].en=this.checked\"><span>\xd0\x92\xd0\xba\xd0\xbb</span></div>'\n    +'<div class=\"f\"><label>IP</label><input type=\"text\" value=\"'+d.ip+'\" onchange=\"tcpd['+i+'].ip=this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:80px\"><label>\xd0\x9f\xd0\xbe\xd1\x80\xd1\x82</label><input type=\"number\" value=\"'+d.port+'\" onchange=\"tcpd['+i+'].port=+this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:80px\"><label>Unit ID</label><input type=\"numbe";
        send(sn,(uint8_t*)p42,(uint16_t)sizeof(p42)-1); IWDG->KR=0xAAAA;}
        {static const char p43[]="r\" value=\"'+d.uid+'\" onchange=\"tcpd['+i+'].uid=+this.value\"></div>'\n    +'<div class=\"f\"><label>\xd0\x98\xd0\xbc\xd1\x8f</label><input type=\"text\" value=\"'+d.nm+'\" onchange=\"tcpd['+i+'].nm=this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:70px\"><label>\xd0\x95\xd0\xb4.</label><input type=\"text\" value=\"'+d.un+'\" onchange=\"tcpd['+i+'].un=this.value\"></div>'\n    +'</div><div class=\"r\">'\n    +'<div class=\"f\" style=\"max-width:80px\"><label>Func</label><select onchange=\"tcpd['+i+'].fc=+this.value\"><option'+(d.fc==3?' selected':'')+' va";
        send(sn,(uint8_t*)p43,(uint16_t)sizeof(p43)-1); IWDG->KR=0xAAAA;}
        {static const char p44[]="lue=\"3\">03</option><option'+(d.fc==4?' selected':'')+' value=\"4\">04</option></select></div>'\n    +'<div class=\"f\" style=\"max-width:100px\"><label>Reg start</label><input type=\"text\" value=\"0x'+d.rs.toString(16).toUpperCase()+'\" onchange=\"tcpd['+i+'].rs=parseInt(this.value)\"></div>'\n    +'<div class=\"f\" style=\"max-width:80px\"><label>Count</label><input type=\"number\" value=\"'+d.rc+'\" onchange=\"tcpd['+i+'].rc=+this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:130px\"><label>Data type</label><select oncha";
        send(sn,(uint8_t*)p44,(uint16_t)sizeof(p44)-1); IWDG->KR=0xAAAA;}
        {static const char p45[]="nge=\"tcpd['+i+'].dt=this.value\">'\n    +DT.map(function(t){return'<option'+(d.dt===t?' selected':'')+'>'+t+'</option>'}).join('')\n    +'</select></div>'\n    +'<div class=\"f\" style=\"max-width:70px\"><label>\xd0\x9c\xd0\xb0\xd1\x81\xd1\x88\xd1\x82\xd0\xb0\xd0\xb1</label><input type=\"number\" step=\"any\" value=\"'+d.sc+'\" onchange=\"tcpd['+i+'].sc=+this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:70px\"><label>\xd0\xa1\xd0\xbc\xd0\xb5\xd1\x89\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5</label><input type=\"number\" step=\"any\" value=\"'+d.of+'\" onchange=\"tcpd['+i+'].of=+this.value\"></div>'\n    +'<div class=\"f\" s";
        send(sn,(uint8_t*)p45,(uint16_t)sizeof(p45)-1); IWDG->KR=0xAAAA;}
        {static const char p46[]="tyle=\"max-width:70px\"><label>CH idx</label><input type=\"number\" value=\"'+d.ci+'\" onchange=\"tcpd['+i+'].ci=+this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:110px\"><label>Poll timeout(\xd0\xbc\xd1\x81)</label><input type=\"number\" value=\"'+d.pms+'\" onchange=\"tcpd['+i+'].pms=+this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:120px\"><label>Conn timeout(\xd0\xbc\xd1\x81)</label><input type=\"number\" value=\"'+d.cms+'\" onchange=\"tcpd['+i+'].cms=+this.value\"></div>'\n    +'<div class=\"f\" style=\"max-width:100px\"><label>W5500";
        send(sn,(uint8_t*)p46,(uint16_t)sizeof(p46)-1); IWDG->KR=0xAAAA;}
        {static const char p47[]=" \xd1\x81\xd0\xbe\xd0\xba\xd0\xb5\xd1\x82</label><input type=\"number\" value=\"'+d.sk+'\" onchange=\"tcpd['+i+'].sk=+this.value\"></div>'\n    +'</div></div>';\n}\n\nfunction addTD(){if(tcpd.length<8){tcpd.push({en:false,ip:'',port:502,uid:1,nm:'',un:'',fc:3,rs:0,rc:1,dt:'INT16',sc:1,of:0,ci:0,pms:1000,cms:3000,sk:1});rndTD()}}\nfunction delTD(i){tcpd.splice(i,1);rndTD()}\nfunction rndTD(){var h='';tcpd.forEach(function(_,i){h+=tcpDH(i)});document.getElementById('tcp_devs').innerHTML=h}\n\nvar regs=[];\nfunction regR(i){\n  var r=regs[i];\n  return '<t";
        send(sn,(uint8_t*)p47,(uint16_t)sizeof(p47)-1); IWDG->KR=0xAAAA;}
        {static const char p48[]="r><td><input type=\"number\" value=\"'+r.ad+'\" style=\"width:55px\" onchange=\"regs['+i+'].ad=+this.value\"></td>'\n    +'<td><select style=\"font-size:11px\" onchange=\"regs['+i+'].src=this.value\">'+SRC.map(function(s){return'<option'+(r.src===s?' selected':'')+'>'+s+'</option>'}).join('')+'</select></td>'\n    +'<td><select style=\"font-size:11px\" onchange=\"regs['+i+'].dt=this.value\">'+DT.map(function(t){return'<option'+(r.dt===t?' selected':'')+'>'+t+'</option>'}).join('')+'</select></td>'\n    +'<td><input type=\"numb";
        send(sn,(uint8_t*)p48,(uint16_t)sizeof(p48)-1); IWDG->KR=0xAAAA;}
        {static const char p49[]="er\" step=\"any\" value=\"'+r.sc+'\" style=\"width:50px\" onchange=\"regs['+i+'].sc=+this.value\"></td>'\n    +'<td><input type=\"text\" value=\"'+r.un+'\" style=\"width:35px\" onchange=\"regs['+i+'].un=this.value\"></td>'\n    +'<td><input type=\"text\" value=\"'+r.nm+'\" style=\"width:70px\" onchange=\"regs['+i+'].nm=this.value\"></td>'\n    +'<td><button style=\"background:none;border:none;color:#f87171;cursor:pointer\" onclick=\"delReg('+i+')\">\xe2\x9c\x95</button></td></tr>';\n}\nfunction addReg(){regs.push({ad:0,src:'\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba 0',dt:'INT16";
        send(sn,(uint8_t*)p49,(uint16_t)sizeof(p49)-1); IWDG->KR=0xAAAA;}
        {static const char p50[]="',sc:1,un:'',nm:''});rndRegs()}\nfunction delReg(i){regs.splice(i,1);rndRegs()}\nfunction rndRegs(){var h='';regs.forEach(function(_,i){h+=regR(i)});document.getElementById('regtbl').innerHTML=h}\n\nfunction initSns(n){\n  var a='',b='';\n  for(var i=0;i<n;i++){\n    a+='<div class=\"f\" style=\"margin-bottom:4px\"><label>\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba '+i+' \xd0\xbc\xd0\xb8\xd0\xbd</label><input type=\"number\" step=\"any\" onchange=\"c(\\'sensor_min'+i+'\\',+this.value)\"></div>';\n    b+='<div class=\"f\" style=\"margin-bottom:4px\"><label>\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba '+i+' \xd0\xbc\xd0\xb0";
        send(sn,(uint8_t*)p50,(uint16_t)sizeof(p50)-1); IWDG->KR=0xAAAA;}
        {static const char p51[]="\xd0\xba\xd1\x81</label><input type=\"number\" step=\"any\" onchange=\"c(\\'sensor_max'+i+'\\',+this.value)\"></div>';\n  }\n  document.getElementById('sns_min').innerHTML=a;\n  document.getElementById('sns_max').innerHTML=b;\n}\n\nfunction initTZ(){\n  var h='';for(var i=-12;i<=14;i++)h+='<option value=\"'+i+'\"'+(i===3?' selected':'')+'>'+(i>=0?'+':'')+i+'</option>';\n  document.getElementById('tz_off').innerHTML=h;\n}\n\nfunction showMsg(ok,t){var m=document.getElementById('msg');m.className='msg '+(ok?'ok':'er');m.textContent=t;m.style";
        send(sn,(uint8_t*)p51,(uint16_t)sizeof(p51)-1); IWDG->KR=0xAAAA;}
        {static const char p52[]=".display='inline';setTimeout(function(){m.style.display='none'},3000)}\n\nfunction save(){\n  var out=Object.assign({},cfg,{rtu:rtu,tcp_devs:tcpd,regs:regs,chain_order:cord,co_en:coen});\n  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(out)})\n    .then(function(r){showMsg(r.ok,'\xe2\x9c\x93 \xd0\xa1\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbd\xd0\xbe')}).catch(function(){showMsg(false,'\xe2\x9c\x97 \xd0\x9e\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0')});\n}\n\nfunction rst(){\n  if(confirm('\xd0\xa1\xd0\xb1\xd1\x80\xd0\xbe\xd1\x81\xd0\xb8\xd1\x82\xd1\x8c \xd0\xba\xd0\xbe\xd0\xbd\xd1\x84\xd0\xb8\xd0\xb3\xd1\x83\xd1\x80\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8e?'))\n    fetch('/api/config',";
        send(sn,(uint8_t*)p52,(uint16_t)sizeof(p52)-1); IWDG->KR=0xAAAA;}
        {static const char p53[]="{method:'POST',headers:{'Content-Type':'application/json'},body:'{\"__reset\":true}'})\n      .then(function(r){showMsg(r.ok,'\xe2\x9c\x93 \xd0\xa1\xd0\xb1\xd1\x80\xd0\xbe\xd1\x81');if(r.ok)location.reload()}).catch(function(){showMsg(false,'\xe2\x9c\x97 \xd0\x9e\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0')});\n}\n\nfunction exp(){\n  var b=new Blob([JSON.stringify(Object.assign({},cfg,{rtu:rtu,tcp_devs:tcpd,regs:regs}),null,2)],{type:'application/json'});\n  var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='config.jsn';a.click();\n}\n\nwindow.addEventListener('DOMContentLoaded";
        send(sn,(uint8_t*)p53,(uint16_t)sizeof(p53)-1); IWDG->KR=0xAAAA;}
        {static const char p54[]="',function(){\n  initTZ();rndOrd();\n  for(var p=0;p<3;p++){rtu.push({en:false,baud:9600,sb:1,par:'None',rms:500,fms:3,devs:[]});document.getElementById('tp'+p).innerHTML=rtuH(p)}\n  initSns(4);\n  fetch('/api/sensors').then(function(r){return r.json()}).then(function(d){initSns(d.count||4)}).catch(function(){});\n  fetch('/api/config').then(function(r){return r.json()}).then(function(d){\n    cfg=d;\n    var bk=['ch_eth','ch_gsm','ch_wifi','ch_iridium','chain_mode','deep_sleep_enabled','schedule_enabled','mtcpm_e";
        send(sn,(uint8_t*)p54,(uint16_t)sizeof(p54)-1); IWDG->KR=0xAAAA;}
        {static const char p55[]="n','mtcps_en','ntp_enabled','web_auth_enabled','web_exclusive_mode','alerts_enabled','alert_on_channel_fail','alert_on_sensor_fail','mqtt_tls','eth_dhcp'];\n    bk.forEach(function(k){var e=document.getElementById(k);if(e)e.checked=!!d[k]});\n    var tx=['tb_host','tb_token','tb_port','mqtt_host','mqtt_port','mqtt_user','mqtt_topic','webhook_url','poll_interval_s','send_interval_s','backup_retry_s','avg_count','deep_sleep_s','schedule_start','schedule_stop','sl_port','sl_uid','sl_sock','sl_ctms','eth_ip','eth";
        send(sn,(uint8_t*)p55,(uint16_t)sizeof(p55)-1); IWDG->KR=0xAAAA;}
        {static const char p56[]="_sn','eth_gw','eth_dns','gsm_apn','gsm_user','wifi_ssid','ntp_server','web_user','web_port','web_idle_timeout_s','batt_low','alert_webhook_url','channel_timeout_s','channel_retry_s','channel_max_retries'];\n    tx.forEach(function(k){var e=document.getElementById(k);if(e&&d[k]!==undefined)e.value=d[k]});\n    if(d.proto)sp(d.proto);\n    if(d.chain_order){cord=d.chain_order;coen=d.co_en||coen;rndOrd()}\n    if(d.tcp_devs){tcpd=d.tcp_devs;rndTD()}\n    if(d.regs){regs=d.regs;rndRegs()}\n    sh('ds_f',!!d.deep_slee";
        send(sn,(uint8_t*)p56,(uint16_t)sizeof(p56)-1); IWDG->KR=0xAAAA;}
        {static const char p57[]="p_enabled);\n    sh('eth_st',!d.eth_dhcp);\n  }).catch(function(){});\n});\n</script></body></html>\n";
        send(sn,(uint8_t*)p57,(uint16_t)sizeof(p57)-1); IWDG->KR=0xAAAA;}
    }
}

void WebServer::handleIndex(uint8_t sn){
    if(serveFile(sn,"/index.html","text/html")) return;
    // Embedded fallback — /index.html not found on SD
    {
        char hdr[128];
        int hl=std::snprintf(hdr,sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n"
            "Content-Length: 17677\r\nAccess-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n");
        send(sn,(uint8_t*)hdr,(uint16_t)hl);
        {static const char p0[]="<!DOCTYPE html>\n<html lang=\"ru\">\n<head>\n<meta charset=\"UTF-8\">\n<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>MonSet \xe2\x80\x94 \xd0\xa2\xd0\xb5\xd1\x81\xd1\x82</title>\n<style>\n:root{\n  --bg:#0e1117;--surface:#161b22;--border:#30363d;\n  --accent:#2dd4bf;--text:#c9d1d9;--muted:#8b949e;\n  --green:#3fb950;--yellow:#d29922;--red:#f85149;\n}\n*{box-sizing:border-box;margin:0;padding:0}\nbody{background:var(--bg);color:var(--text);font-family:system-ui,'Segoe UI',Inter,sans-serif;font-size:14px;min-width:320px}\na{color";
        send(sn,(uint8_t*)p0,(uint16_t)sizeof(p0)-1); IWDG->KR=0xAAAA;}
        {static const char p1[]=":var(--text);text-decoration:none}\n/* NAV */\nnav{display:flex;align-items:center;gap:0;background:var(--surface);border-bottom:1px solid var(--border);padding:0 16px;height:44px;position:sticky;top:0;z-index:10}\n.brand{font-weight:700;letter-spacing:.04em;margin-right:auto;font-size:15px;white-space:nowrap}\n.brand-dot{color:var(--red)}\n.nav-tabs{display:flex;gap:0}\n.nav-tab{padding:0 14px;height:44px;display:flex;align-items:center;font-size:13px;border-bottom:2px solid transparent;color:var(--muted);transi";
        send(sn,(uint8_t*)p1,(uint16_t)sizeof(p1)-1); IWDG->KR=0xAAAA;}
        {static const char p2[]="tion:color .15s}\n.nav-tab:hover{color:var(--text)}\n.nav-tab.active{color:var(--accent);border-bottom-color:var(--accent)}\n/* STATUS BAR */\n.status-bar{background:var(--surface);border-bottom:1px solid var(--border);padding:6px 16px;font-size:12px;font-family:'Courier New',Courier,monospace;display:flex;align-items:center;gap:8px}\n.dot-red{color:var(--red)}\n.dot-green{color:var(--green)}\n/* MAIN */\nmain{padding:16px;max-width:900px;margin:0 auto;display:flex;flex-direction:column;gap:16px}\n/* CARD */\n.card{b";
        send(sn,(uint8_t*)p2,(uint16_t)sizeof(p2)-1); IWDG->KR=0xAAAA;}
        {static const char p3[]="ackground:var(--surface);border:1px solid var(--border);border-radius:6px;overflow:hidden}\n.card-head{padding:10px 16px;border-bottom:1px solid var(--border);font-size:11px;letter-spacing:.1em;font-weight:700;color:var(--muted);display:flex;align-items:center;gap:8px}\n.card-body{padding:14px 16px}\n/* SENSORS */\n.sensor-row{display:flex;align-items:center;gap:8px;padding:6px 0;border-bottom:1px solid var(--border)}\n.sensor-row:last-child{border-bottom:0}\n.sensor-name{flex:1;color:var(--muted);font-size:13px}";
        send(sn,(uint8_t*)p3,(uint16_t)sizeof(p3)-1); IWDG->KR=0xAAAA;}
        {static const char p4[]="\n.sensor-val{font-family:'Courier New',Courier,monospace;font-size:16px;font-weight:700;color:var(--text);min-width:90px;text-align:right}\n.sensor-unit{font-family:'Courier New',Courier,monospace;font-size:11px;color:var(--muted);min-width:32px}\n.badge{font-size:10px;padding:2px 6px;border-radius:3px;font-weight:700;letter-spacing:.05em}\n.badge-ok{background:#1a3a2a;color:var(--green)}\n.badge-err{background:#3a1a1a;color:var(--red)}\n/* BATTERY */\n.batt-row{display:flex;align-items:center;gap:10px;padding:8p";
        send(sn,(uint8_t*)p4,(uint16_t)sizeof(p4)-1); IWDG->KR=0xAAAA;}
        {static const char p5[]="x 0}\n.batt-label{color:var(--muted);font-size:13px;min-width:70px}\n.batt-bar{flex:1;height:8px;background:#0e1117;border-radius:4px;border:1px solid var(--border);overflow:hidden}\n.batt-fill{height:100%;background:var(--accent);border-radius:4px;transition:width .4s}\n.batt-pct{font-family:'Courier New',Courier,monospace;font-size:13px;min-width:36px;text-align:right}\n.batt-v{font-family:'Courier New',Courier,monospace;font-size:12px;color:var(--muted);min-width:48px}\n.ts-row{padding-top:8px;font-family:'Cou";
        send(sn,(uint8_t*)p5,(uint16_t)sizeof(p5)-1); IWDG->KR=0xAAAA;}
        {static const char p6[]="rier New',Courier,monospace;font-size:11px;color:var(--muted)}\n/* CHANNELS */\n.ch-table{width:100%;border-collapse:collapse}\n.ch-table td{padding:7px 8px;font-size:13px;border-bottom:1px solid var(--border)}\n.ch-table tr:last-child td{border-bottom:0}\n.ch-dot{font-size:14px;width:20px}\n.ch-name{font-weight:600}\n.ch-status{font-family:'Courier New',Courier,monospace;font-size:12px;font-weight:700;letter-spacing:.05em}\n.ch-prio{color:var(--muted);font-size:12px}\n.st-active{color:var(--green)}\n.st-standby{colo";
        send(sn,(uint8_t*)p6,(uint16_t)sizeof(p6)-1); IWDG->KR=0xAAAA;}
        {static const char p7[]="r:var(--yellow)}\n.st-disabled{color:#444}\n/* ACTIONS */\n.btn-test{width:100%;padding:14px;background:#1a3028;border:2px solid var(--accent);border-radius:5px;color:var(--accent);font-size:15px;font-weight:700;cursor:pointer;letter-spacing:.04em;transition:background .15s}\n.btn-test:hover{background:#1f3d32}\n.btn-test:active{background:#0e1117}\n.btn-test:disabled{opacity:.5;cursor:not-allowed}\n.progress-steps{display:flex;gap:6px;margin-top:12px;flex-wrap:wrap}\n.step{font-size:11px;padding:3px 8px;border-rad";
        send(sn,(uint8_t*)p7,(uint16_t)sizeof(p7)-1); IWDG->KR=0xAAAA;}
        {static const char p8[]="ius:3px;font-family:'Courier New',Courier,monospace;letter-spacing:.05em;background:#0e1117;color:#444;border:1px solid #222}\n.step.done{background:#1a3a2a;color:var(--green);border-color:#2a4a3a}\n.step.active{background:#1a2a3a;color:var(--accent);border-color:var(--accent)}\n.step.fail{background:#3a1a1a;color:var(--red);border-color:#5a2a2a}\n.result-row{margin-top:12px;font-family:'Courier New',Courier,monospace;font-size:12px;color:var(--muted);display:flex;gap:16px;flex-wrap:wrap}\n.result-row span b{col";
        send(sn,(uint8_t*)p8,(uint16_t)sizeof(p8)-1); IWDG->KR=0xAAAA;}
        {static const char p9[]="or:var(--text)}\n.web-status{margin-top:14px;padding:8px 12px;border-radius:4px;border:1px solid var(--border);font-size:13px;font-family:'Courier New',Courier,monospace}\n.ws-active{color:var(--red)}\n.ws-online{color:var(--green)}\n.err-msg{color:var(--red);font-size:12px;margin-top:8px;font-family:'Courier New',Courier,monospace}\n/* REFRESH indicator */\n.refresh-dot{width:6px;height:6px;border-radius:50%;background:var(--muted);display:inline-block;margin-left:auto;transition:background .2s}\n.refresh-dot.pul";
        send(sn,(uint8_t*)p9,(uint16_t)sizeof(p9)-1); IWDG->KR=0xAAAA;}
        {static const char p10[]="se{background:var(--accent)}\n</style>\n</head>\n<body>\n\n<nav>\n  <span class=\"brand\">Mon<span style=\"color:var(--accent)\">Set</span></span>\n  <div class=\"nav-tabs\">\n    <a class=\"nav-tab\" href=\"/config\">\xd0\x9a\xd0\xbe\xd0\xbd\xd1\x84\xd0\xb8\xd0\xb3</a>\n    <a class=\"nav-tab active\" href=\"/\">\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82</a>\n    <a class=\"nav-tab\" href=\"/logs\">\xd0\x9b\xd0\xbe\xd0\xb3</a>\n  </div>\n</nav>\n\n<div class=\"status-bar\">\n  <span class=\"dot-red\" id=\"hdr-dot\">\xe2\x97\x8f</span>\n  <span id=\"hdr-text\">WEB ACTIVE \xe2\x80\x94 \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb4\xd0\xb0\xd1\x87\xd0\xb0 \xd0\xbf\xd1\x80\xd0\xb8\xd0\xbe\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xb0</span>\n</div>\n\n<main>\n\n";
        send(sn,(uint8_t*)p10,(uint16_t)sizeof(p10)-1); IWDG->KR=0xAAAA;}
        {static const char p11[]="  <!-- SECTION 1: \xd0\xa2\xd0\x95\xd0\x9a\xd0\xa3\xd0\xa9\xd0\x95\xd0\x95 \xd0\x98\xd0\x97\xd0\x9c\xd0\x95\xd0\xa0\xd0\x95\xd0\x9d\xd0\x98\xd0\x95 -->\n  <div class=\"card\">\n    <div class=\"card-head\">\n      \xd0\xa2\xd0\x95\xd0\x9a\xd0\xa3\xd0\xa9\xd0\x95\xd0\x95 \xd0\x98\xd0\x97\xd0\x9c\xd0\x95\xd0\xa0\xd0\x95\xd0\x9d\xd0\x98\xd0\x95\n      <span class=\"refresh-dot\" id=\"rdot1\"></span>\n    </div>\n    <div class=\"card-body\">\n      <div id=\"sensors-list\">\n        <div class=\"sensor-row\">\n          <span class=\"sensor-name\">\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba 0</span>\n          <span class=\"sensor-val\" style=\"color:var(--muted)\">\xe2\x80\x94</span>\n          <span class=\"sensor-unit\"></span>\n          <span class=\"badge\">\xe2\x80\xa6</span";
        send(sn,(uint8_t*)p11,(uint16_t)sizeof(p11)-1); IWDG->KR=0xAAAA;}
        {static const char p12[]=">\n        </div>\n        <div class=\"sensor-row\">\n          <span class=\"sensor-name\">\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba 1</span>\n          <span class=\"sensor-val\" style=\"color:var(--muted)\">\xe2\x80\x94</span>\n          <span class=\"sensor-unit\"></span>\n          <span class=\"badge\">\xe2\x80\xa6</span>\n        </div>\n        <div class=\"sensor-row\">\n          <span class=\"sensor-name\">\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba 2</span>\n          <span class=\"sensor-val\" style=\"color:var(--muted)\">\xe2\x80\x94</span>\n          <span class=\"sensor-unit\"></span>\n          <span class=\"";
        send(sn,(uint8_t*)p12,(uint16_t)sizeof(p12)-1); IWDG->KR=0xAAAA;}
        {static const char p13[]="badge\">\xe2\x80\xa6</span>\n        </div>\n        <div class=\"sensor-row\">\n          <span class=\"sensor-name\">\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba 3</span>\n          <span class=\"sensor-val\" style=\"color:var(--muted)\">\xe2\x80\x94</span>\n          <span class=\"sensor-unit\"></span>\n          <span class=\"badge\">\xe2\x80\xa6</span>\n        </div>\n        <div class=\"batt-row\">\n          <span class=\"batt-label\">\xd0\x91\xd0\xb0\xd1\x82\xd0\xb0\xd1\x80\xd0\xb5\xd1\x8f</span>\n          <div class=\"batt-bar\"><div class=\"batt-fill\" id=\"batt-fill\" style=\"width:0%\"></div></div>\n          <span class=\"batt";
        send(sn,(uint8_t*)p13,(uint16_t)sizeof(p13)-1); IWDG->KR=0xAAAA;}
        {static const char p14[]="-pct\" id=\"batt-pct\">\xe2\x80\x94%</span>\n          <span class=\"batt-v\" id=\"batt-v\">\xe2\x80\x94V</span>\n        </div>\n      </div>\n      <div class=\"ts-row\" id=\"sens-ts\">\xd0\x9e\xd0\xb1\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xbe: \xe2\x80\x94</div>\n      <div class=\"err-msg\" id=\"sens-err\" style=\"display:none\"></div>\n    </div>\n  </div>\n\n  <!-- SECTION 2: \xd0\xa1\xd0\xa2\xd0\x90\xd0\xa2\xd0\xa3\xd0\xa1 \xd0\x9a\xd0\x90\xd0\x9d\xd0\x90\xd0\x9b\xd0\x9e\xd0\x92 -->\n  <div class=\"card\">\n    <div class=\"card-head\">\n      \xd0\xa1\xd0\xa2\xd0\x90\xd0\xa2\xd0\xa3\xd0\xa1 \xd0\x9a\xd0\x90\xd0\x9d\xd0\x90\xd0\x9b\xd0\x9e\xd0\x92\n      <span class=\"refresh-dot\" id=\"rdot2\"></span>\n    </div>\n    <div class=\"card-body\" style=\"padding:0";
        send(sn,(uint8_t*)p14,(uint16_t)sizeof(p14)-1); IWDG->KR=0xAAAA;}
        {static const char p15[]="\">\n      <table class=\"ch-table\">\n        <tbody id=\"channels-tbody\">\n          <tr>\n            <td class=\"ch-dot st-active\">\xe2\x97\x8f</td>\n            <td class=\"ch-name\">Ethernet W5500</td>\n            <td class=\"ch-status st-active\">ACTIVE</td>\n            <td class=\"ch-prio\">\xd0\xbf\xd1\x80\xd0\xb8\xd0\xbe\xd1\x80\xd0\xb8\xd1\x82\xd0\xb5\xd1\x82 1</td>\n          </tr>\n          <tr>\n            <td class=\"ch-dot st-standby\">\xe2\x97\x8b</td>\n            <td class=\"ch-name\">GSM Air780E</td>\n            <td class=\"ch-status st-standby\">STANDBY</td>\n            <td class=";
        send(sn,(uint8_t*)p15,(uint16_t)sizeof(p15)-1); IWDG->KR=0xAAAA;}
        {static const char p16[]="\"ch-prio\">\xd0\xbf\xd1\x80\xd0\xb8\xd0\xbe\xd1\x80\xd0\xb8\xd1\x82\xd0\xb5\xd1\x82 2</td>\n          </tr>\n          <tr>\n            <td class=\"ch-dot st-disabled\">\xe2\x97\x8b</td>\n            <td class=\"ch-name\">WiFi ESP8266</td>\n            <td class=\"ch-status st-disabled\">DISABLED</td>\n            <td class=\"ch-prio\">\xe2\x80\x94</td>\n          </tr>\n          <tr>\n            <td class=\"ch-dot st-disabled\">\xe2\x97\x8b</td>\n            <td class=\"ch-name\">Iridium SBD</td>\n            <td class=\"ch-status st-disabled\">DISABLED</td>\n            <td class=\"ch-prio\">\xe2\x80\x94</td>\n         ";
        send(sn,(uint8_t*)p16,(uint16_t)sizeof(p16)-1); IWDG->KR=0xAAAA;}
        {static const char p17[]=" </tr>\n          <tr>\n            <td class=\"ch-dot st-disabled\">\xe2\x97\x8b</td>\n            <td class=\"ch-name\">Modbus TCP Master</td>\n            <td class=\"ch-status st-disabled\">0 \xd1\x83\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd1\x81\xd1\x82\xd0\xb2</td>\n            <td class=\"ch-prio\">\xe2\x80\x94</td>\n          </tr>\n          <tr>\n            <td class=\"ch-dot st-active\">\xe2\x97\x8b</td>\n            <td class=\"ch-name\">Modbus TCP Slave</td>\n            <td class=\"ch-status\" style=\"color:var(--accent)\">\xd1\x81\xd0\xbb\xd1\x83\xd1\x88\xd0\xb0\xd0\xb5\xd1\x82 :502</td>\n            <td class=\"ch-prio\">\xe2\x80\x94</td>\n       ";
        send(sn,(uint8_t*)p17,(uint16_t)sizeof(p17)-1); IWDG->KR=0xAAAA;}
        {static const char p18[]="   </tr>\n        </tbody>\n      </table>\n      <div class=\"err-msg\" id=\"ch-err\" style=\"display:none;padding:8px 16px\"></div>\n    </div>\n  </div>\n\n  <!-- SECTION 3: \xd0\x94\xd0\x95\xd0\x99\xd0\xa1\xd0\xa2\xd0\x92\xd0\x98\xd0\xaf -->\n  <div class=\"card\">\n    <div class=\"card-head\">\xd0\x94\xd0\x95\xd0\x99\xd0\xa1\xd0\xa2\xd0\x92\xd0\x98\xd0\xaf</div>\n    <div class=\"card-body\">\n      <button class=\"btn-test\" id=\"btn-test\" onclick=\"startTest()\">\xe2\x96\xb6 \xd0\xa2\xd0\x95\xd0\xa1\xd0\xa2 \xe2\x80\x94 \xd0\xbe\xd1\x82\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xb8\xd1\x82\xd1\x8c \xd1\x82\xd0\xb5\xd0\xba\xd1\x83\xd1\x89\xd0\xb5\xd0\xb5 \xd0\xb8\xd0\xb7\xd0\xbc\xd0\xb5\xd1\x80\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 \xd0\xbd\xd0\xb0 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80</button>\n      <div class=\"progress-steps\" id=\"steps\">\n        <span clas";
        send(sn,(uint8_t*)p18,(uint16_t)sizeof(p18)-1); IWDG->KR=0xAAAA;}
        {static const char p19[]="s=\"step\" id=\"s-wait\">\xd0\x9e\xd0\x96\xd0\x98\xd0\x94\xd0\x90\xd0\x9d\xd0\x98\xd0\x95</span>\n        <span class=\"step\" id=\"s-read\">\xd0\xa7\xd0\xa2\xd0\x95\xd0\x9d\xd0\x98\xd0\x95</span>\n        <span class=\"step\" id=\"s-build\">\xd0\xa1\xd0\x91\xd0\x9e\xd0\xa0\xd0\x9a\xd0\x90</span>\n        <span class=\"step\" id=\"s-send\">\xd0\x9e\xd0\xa2\xd0\x9f\xd0\xa0\xd0\x90\xd0\x92\xd0\x9a\xd0\x90</span>\n        <span class=\"step\" id=\"s-done\">\xd0\xa3\xd0\xa1\xd0\x9f\xd0\x95\xd0\xa5/\xd0\x9e\xd0\xa8\xd0\x98\xd0\x91\xd0\x9a\xd0\x90</span>\n      </div>\n      <div class=\"result-row\" id=\"result-row\" style=\"display:none\">\n        <span>\xd0\x9a\xd0\xb0\xd0\xbd\xd0\xb0\xd0\xbb: <b id=\"r-ch\">\xe2\x80\x94</b></span>\n        <span>\xd0\x9e\xd1\x82\xd0\xb2\xd0\xb5\xd1\x82: <b id=\"r-resp\">\xe2\x80\x94</b></span>\n        <span>\xd0\x92\xd1\x80\xd0\xb5\xd0\xbc\xd1\x8f";
        send(sn,(uint8_t*)p19,(uint16_t)sizeof(p19)-1); IWDG->KR=0xAAAA;}
        {static const char p20[]=": <b id=\"r-time\">\xe2\x80\x94</b></span>\n      </div>\n      <div class=\"err-msg\" id=\"test-err\" style=\"display:none\"></div>\n      <div class=\"web-status\" id=\"web-status\">\n        <span class=\"ws-active\">\xe2\x97\x8f \xd0\x92\xd0\x95\xd0\x91 \xd0\x90\xd0\x9a\xd0\xa2\xd0\x98\xd0\x92\xd0\x95\xd0\x9d \xe2\x80\x94 \xd0\xb7\xd0\xb0\xd0\xb3\xd1\x80\xd1\x83\xd0\xb7\xd0\xba\xd0\xb0...</span>\n      </div>\n    </div>\n  </div>\n\n</main>\n\n<script>\n(function(){\n'use strict';\n\nvar TEST_RUNNING=false;\nvar TEST_TIMER=null;\n\n/* ---- fetch with timeout ---- */\nfunction ftch(url,opts){\n  var ctrl=new AbortController();\n  var tid=setTimeout(function(){ctrl.abort";
        send(sn,(uint8_t*)p20,(uint16_t)sizeof(p20)-1); IWDG->KR=0xAAAA;}
        {static const char p21[]="();},3000);\n  var o=Object.assign({signal:ctrl.signal},opts||{});\n  return fetch(url,o).finally(function(){clearTimeout(tid);});\n}\n\n/* ---- pulse dot ---- */\nfunction pulse(id){\n  var d=document.getElementById(id);\n  if(!d)return;\n  d.classList.add('pulse');\n  setTimeout(function(){d.classList.remove('pulse');},400);\n}\n\n/* ---- SENSORS ---- */\nfunction renderSensors(data){\n  var rows=document.querySelectorAll('#sensors-list .sensor-row');\n  var sensors=data.sensors||[];\n  for(var i=0;i<4;i++){\n    var row=r";
        send(sn,(uint8_t*)p21,(uint16_t)sizeof(p21)-1); IWDG->KR=0xAAAA;}
        {static const char p22[]="ows[i];if(!row)continue;\n    var s=sensors[i]||{};\n    var nm=row.querySelector('.sensor-name');\n    var vl=row.querySelector('.sensor-val');\n    var un=row.querySelector('.sensor-unit');\n    var bd=row.querySelector('.badge');\n    nm.textContent=s.name||('\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba '+i);\n    var valid=s.valid!==false&&s.value!==null&&s.value!==undefined;\n    vl.textContent=valid?s.value:'ERR';\n    vl.style.color=valid?'var(--text)':'var(--red)';\n    un.textContent=s.unit||'';\n    bd.textContent=valid?'OK':'ERR';\n    bd.";
        send(sn,(uint8_t*)p22,(uint16_t)sizeof(p22)-1); IWDG->KR=0xAAAA;}
        {static const char p23[]="className='badge '+(valid?'badge-ok':'badge-err');\n  }\n  var bp=data.battery||{};\n  var pct=bp.percent!=null?bp.percent:null;\n  var volt=bp.voltage!=null?bp.voltage:null;\n  document.getElementById('batt-fill').style.width=(pct!=null?pct:0)+'%';\n  document.getElementById('batt-pct').textContent=(pct!=null?pct+'%':'\xe2\x80\x94%');\n  document.getElementById('batt-v').textContent=(volt!=null?volt.toFixed(2)+'V':'\xe2\x80\x94V');\n  document.getElementById('sens-ts').textContent='\xd0\x9e\xd0\xb1\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xbe: '+(data.timestamp||'\xe2\x80\x94');\n}\n\n";
        send(sn,(uint8_t*)p23,(uint16_t)sizeof(p23)-1); IWDG->KR=0xAAAA;}
        {static const char p24[]="function loadSensors(){\n  ftch('/api/sensors').then(function(r){return r.json();}).then(function(d){\n    document.getElementById('sens-err').style.display='none';\n    renderSensors(d);\n    pulse('rdot1');\n  }).catch(function(){\n    var e=document.getElementById('sens-err');\n    e.textContent='\xe2\x9a\xa0 \xd0\x9d\xd0\xb5\xd1\x82 \xd1\x81\xd0\xb2\xd1\x8f\xd0\xb7\xd0\xb8 \xd1\x81 \xd1\x83\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd1\x81\xd1\x82\xd0\xb2\xd0\xbe\xd0\xbc';\n    e.style.display='block';\n    pulse('rdot1');\n  });\n}\n\n/* ---- CHANNELS ---- */\nvar CH_STATUS_CLS={\n  'ACTIVE':'st-active','STANDBY':'st-standby','DISABLED':'st-disa";
        send(sn,(uint8_t*)p24,(uint16_t)sizeof(p24)-1); IWDG->KR=0xAAAA;}
        {static const char p25[]="bled'\n};\nvar CH_DOT={\n  'ACTIVE':'\xe2\x97\x8f','STANDBY':'\xe2\x97\x8b','DISABLED':'\xe2\x97\x8b'\n};\n\nfunction renderChannels(data){\n  var chs=data.channels||[];\n  if(!chs.length)return;\n  var tbody=document.getElementById('channels-tbody');\n  tbody.innerHTML='';\n  for(var i=0;i<chs.length;i++){\n    var c=chs[i];\n    var st=c.status||'DISABLED';\n    var cls=CH_STATUS_CLS[st]||'st-disabled';\n    var dot=CH_DOT[st]||'\xe2\x97\x8b';\n    var tr=document.createElement('tr');\n    tr.innerHTML='<td class=\"ch-dot '+cls+'\">'+dot+'</td>'+\n      '<td c";
        send(sn,(uint8_t*)p25,(uint16_t)sizeof(p25)-1); IWDG->KR=0xAAAA;}
        {static const char p26[]="lass=\"ch-name\">'+esc(c.name||'')+'</td>'+\n      '<td class=\"ch-status '+cls+'\">'+esc(c.label||st)+'</td>'+\n      '<td class=\"ch-prio\">'+esc(c.priority||'\xe2\x80\x94')+'</td>';\n    tbody.appendChild(tr);\n  }\n}\n\nfunction loadChannels(){\n  ftch('/api/channels').then(function(r){return r.json();}).then(function(d){\n    document.getElementById('ch-err').style.display='none';\n    renderChannels(d);\n    pulse('rdot2');\n  }).catch(function(){\n    var e=document.getElementById('ch-err');\n    e.textContent='\xe2\x9a\xa0 \xd0\x9d\xd0\xb5\xd1\x82 \xd1\x81\xd0\xb2\xd1\x8f";
        send(sn,(uint8_t*)p26,(uint16_t)sizeof(p26)-1); IWDG->KR=0xAAAA;}
        {static const char p27[]="\xd0\xb7\xd0\xb8 \xd1\x81 \xd1\x83\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd1\x81\xd1\x82\xd0\xb2\xd0\xbe\xd0\xbc';\n    e.style.display='block';\n    pulse('rdot2');\n  });\n}\n\n/* ---- WEB MODE ---- */\nfunction loadWebMode(){\n  ftch('/api/web_mode').then(function(r){return r.json();}).then(function(d){\n    var ws=document.getElementById('web-status');\n    var hd=document.getElementById('hdr-dot');\n    var ht=document.getElementById('hdr-text');\n    if(d.web_active){\n      var rem=d.remaining!=null?' (\xd0\xb5\xd1\x89\xd1\x91 '+d.remaining+' \xd1\x81)':'';\n      ws.innerHTML='<span class=\"ws-active\">\xe2\x97\x8f \xd0\x92\xd0\x95\xd0\x91 \xd0\x90\xd0\x9a\xd0";
        send(sn,(uint8_t*)p27,(uint16_t)sizeof(p27)-1); IWDG->KR=0xAAAA;}
        {static const char p28[]="\xa2\xd0\x98\xd0\x92\xd0\x95\xd0\x9d \xe2\x80\x94 \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb4\xd0\xb0\xd1\x87\xd0\xb0 \xd0\xbf\xd1\x80\xd0\xb8\xd0\xbe\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xb0'+esc(rem)+'</span>';\n      hd.className='dot-red';hd.textContent='\xe2\x97\x8f';\n      ht.textContent='WEB ACTIVE \xe2\x80\x94 \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb4\xd0\xb0\xd1\x87\xd0\xb0 \xd0\xbf\xd1\x80\xd0\xb8\xd0\xbe\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xb0';\n    }else{\n      ws.innerHTML='<span class=\"ws-online\">\xe2\x97\x8b \xd0\x9e\xd0\x9d\xd0\x9b\xd0\x90\xd0\x99\xd0\x9d</span>';\n      hd.className='dot-green';hd.textContent='\xe2\x97\x8b';\n      ht.textContent='\xd0\x9e\xd0\x9d\xd0\x9b\xd0\x90\xd0\x99\xd0\x9d';\n    }\n  }).catch(function(){});\n}\n\n/* ---- TEST ---- */\nvar STEPS=['wait','read','build','send','done'];\nvar STEP_MAP={\n  'i";
        send(sn,(uint8_t*)p28,(uint16_t)sizeof(p28)-1); IWDG->KR=0xAAAA;}
        {static const char p29[]="dle':0,'waiting':0,'reading':1,'building':2,'sending':3,\n  'success':4,'error':4,'fail':4\n};\n\nfunction setStep(state,failed){\n  var idx=STEP_MAP[state]!=null?STEP_MAP[state]:0;\n  var ids=['s-wait','s-read','s-build','s-send','s-done'];\n  var labels=['\xd0\x9e\xd0\x96\xd0\x98\xd0\x94\xd0\x90\xd0\x9d\xd0\x98\xd0\x95','\xd0\xa7\xd0\xa2\xd0\x95\xd0\x9d\xd0\x98\xd0\x95','\xd0\xa1\xd0\x91\xd0\x9e\xd0\xa0\xd0\x9a\xd0\x90','\xd0\x9e\xd0\xa2\xd0\x9f\xd0\xa0\xd0\x90\xd0\x92\xd0\x9a\xd0\x90',\n    failed?'\xd0\x9e\xd0\xa8\xd0\x98\xd0\x91\xd0\x9a\xd0\x90':'\xd0\xa3\xd0\xa1\xd0\x9f\xd0\x95\xd0\xa5'];\n  for(var i=0;i<ids.length;i++){\n    var el=document.getElementById(ids[i]);\n    el.textContent=labels[i];\n    el.className='step';\n    if(i<idx)el.clas";
        send(sn,(uint8_t*)p29,(uint16_t)sizeof(p29)-1); IWDG->KR=0xAAAA;}
        {static const char p30[]="sList.add('done');\n    else if(i===idx){\n      if((state==='success')&&i===4)el.classList.add('done');\n      else if((state==='error'||state==='fail')&&i===4)el.classList.add('fail');\n      else el.classList.add('active');\n    }\n  }\n}\n\nfunction startTest(){\n  if(TEST_RUNNING)return;\n  TEST_RUNNING=true;\n  var btn=document.getElementById('btn-test');\n  btn.disabled=true;\n  document.getElementById('result-row').style.display='none';\n  document.getElementById('test-err').style.display='none';\n  setStep('waitin";
        send(sn,(uint8_t*)p30,(uint16_t)sizeof(p30)-1); IWDG->KR=0xAAAA;}
        {static const char p31[]="g',false);\n  ftch('/api/test_send',{method:'POST'}).then(function(r){\n    if(!r.ok)throw new Error('HTTP '+r.status);\n    return r.json();\n  }).then(function(){\n    TEST_TIMER=setInterval(pollResult,500);\n  }).catch(function(e){\n    showTestErr('\xe2\x9a\xa0 \xd0\x9e\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0 \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0 \xd1\x82\xd0\xb5\xd1\x81\xd1\x82\xd0\xb0: '+e.message);\n    stopTest();\n  });\n}\n\nfunction pollResult(){\n  ftch('/api/test_result').then(function(r){return r.json();}).then(function(d){\n    var st=d.state||'idle';\n    var failed=st==='error'||st==='fail';\n    setSt";
        send(sn,(uint8_t*)p31,(uint16_t)sizeof(p31)-1); IWDG->KR=0xAAAA;}
        {static const char p32[]="ep(st,failed);\n    if(st==='success'||failed){\n      if(d.channel||d.response||d.time_ms!=null){\n        document.getElementById('r-ch').textContent=d.channel||'\xe2\x80\x94';\n        document.getElementById('r-resp').textContent=d.response||'\xe2\x80\x94';\n        document.getElementById('r-time').textContent=d.time_ms!=null?d.time_ms+' \xd0\xbc\xd1\x81':'\xe2\x80\x94';\n        document.getElementById('result-row').style.display='flex';\n      }\n      if(failed&&d.error){\n        var e=document.getElementById('test-err');\n        e.textContent='";
        send(sn,(uint8_t*)p32,(uint16_t)sizeof(p32)-1); IWDG->KR=0xAAAA;}
        {static const char p33[]="\xe2\x9a\xa0 '+d.error;e.style.display='block';\n      }\n      stopTest();\n    }\n  }).catch(function(){\n    showTestErr('\xe2\x9a\xa0 \xd0\x9d\xd0\xb5\xd1\x82 \xd1\x81\xd0\xb2\xd1\x8f\xd0\xb7\xd0\xb8 \xd1\x81 \xd1\x83\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd1\x81\xd1\x82\xd0\xb2\xd0\xbe\xd0\xbc');\n    stopTest();\n  });\n}\n\nfunction stopTest(){\n  TEST_RUNNING=false;\n  if(TEST_TIMER){clearInterval(TEST_TIMER);TEST_TIMER=null;}\n  var btn=document.getElementById('btn-test');\n  btn.disabled=false;\n}\n\nfunction showTestErr(msg){\n  var e=document.getElementById('test-err');\n  e.textContent=msg;e.style.display='block';\n}\n\nfunction esc(s){\n  return Str";
        send(sn,(uint8_t*)p33,(uint16_t)sizeof(p33)-1); IWDG->KR=0xAAAA;}
        {static const char p34[]="ing(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');\n}\n\n/* ---- INIT ---- */\nloadSensors();\nloadChannels();\nloadWebMode();\nsetInterval(loadSensors,5000);\nsetInterval(loadChannels,5000);\nsetInterval(loadWebMode,5000);\n\n})();\n</script>\n</body>\n</html>\n";
        send(sn,(uint8_t*)p34,(uint16_t)sizeof(p34)-1); IWDG->KR=0xAAAA;}
    }
}
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
           std::strcmp(cleanPath,"/index.html")==0)      handleIndex(sn);
        else if(std::strcmp(cleanPath,"/config")==0||
                std::strcmp(cleanPath,"/config.html")==0||
                std::strcmp(cleanPath,"/dashboard")==0)  handleConfig(sn);
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
                // Wait for TX buffer to drain before closing socket
                { uint32_t t0=HAL_GetTick();
                  uint16_t txmax=getSn_TxMAX(HTTP_SOCKET);
                  while(getSn_TX_FSR(HTTP_SOCKET)<txmax &&
                        (HAL_GetTick()-t0)<3000){ IWDG->KR=0xAAAA; HAL_Delay(2); }
                }
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
