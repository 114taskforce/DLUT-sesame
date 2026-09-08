// ====================================================================
// 本地配网 / 调试通道实现。接口契约的唯一出处是 docs/app-api.md 的 1。
//
// 端点(都要带 ?k=<PIN> 或 X-Auth: <PIN>):
//   GET  /status   探测与健康
//   POST /save     写字段(只写提交的那些)
//   POST /apply    立即生效: WiFi → 校园网认证 → 校时 → 门禁 token
//   POST /open     试开门, 回显服务端原文 —— 核对设备编号对不对的主通道
//   GET  /         单页表单(浏览器直接开 192.168.4.1 就能手工操作)
//
// 全程单线程: 所有 handler 都在 loop() 的 provHandleClient() 里跑完, 所以
// /apply 期间设备不会同时处理第二个请求(手机侧表现为该请求耗时几十秒)。
// ====================================================================

#include "prov.h"
#include "app.h"
#include "cloud.h"
#include "config.h"
#include "settings.h"

#include <WiFi.h>
#include <WebServer.h>

#define PROV_AP_IDLE_MS  600000UL     // 热点无人操作 10 分钟后自动关掉
#define PROV_APPLY_GAP   10000UL      // /apply 频控
#define PROV_OPEN_GAP    1500UL       // /open 频控(与 main 的 DOOR_MIN_INTERVAL 双保险)

static WebServer sSrv(80);
static bool sApOn = false;
static unsigned long sLastActivity = 0;
static unsigned long sLastApply = 0;
static unsigned long sLastOpen = 0;
static bool sApRestart = false;       // PIN 改过 → 下个周期用新口令重开热点

// HTTP 表单字段名 → settings 字段名(docs/app-api.md 1.2)
struct ArgMap { const char* http; const char* field; };
static const ArgMap gArgs[] = {
    { "ssid",      "ssid"      },
    { "wifipswd",  "wifipswd"  },
    { "user",      "user"      },
    { "pass",      "pass"      },
    { "code",      "code"      },
    { "cookie",    "cookie"    },
    { "pin",       "pin"       },
    { "bemfakey",  "bemfakey"  },
    { "doortopic", "doorTopic" },
    { "cfgtopic",  "cfgTopic"  },
    { "acktopic",  "ackTopic"  },
};

// ==================== 小工具 ====================

static void jsonEsc(String& out, const String& s)
{
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if ((unsigned char)c < 0x20) out += ' ';   // 其它控制字符换空格, 别破坏 JSON
        else out += c;
    }
}

static void jsonStr(String& out, const char* key, const String& val)
{
    if (out.length() > 1) out += ',';
    out += '"'; out += key; out += "\":\"";
    jsonEsc(out, val);
    out += '"';
}

static void jsonBool(String& out, const char* key, bool v)
{
    if (out.length() > 1) out += ',';
    out += '"'; out += key; out += "\":"; out += (v ? "true" : "false");
}

static void jsonNum(String& out, const char* key, unsigned long v)
{
    if (out.length() > 1) out += ',';
    out += '"'; out += key; out += "\":"; out += String(v);
}

// 已经拼好的 JSON 片段(数组/对象)当一个字段插进去。
// 别在 handler 里手写 j += "\"saved\":" + ... —— 上面几个 helper 都按"out 不是光
// 一个 { 就补逗号"的约定自己加分隔符, 手工拼接会漏掉前导逗号又多出一个尾逗号,
// 发出去就是 {"ok":true"saved":{...},,...} 这种不合法的 JSON。
static void jsonRaw(String& out, const char* key, const String& raw)
{
    if (out.length() > 1) out += ',';
    out += '"'; out += key; out += "\":"; out += raw;
}

static void sendJson(int code, const String& body)
{
    sLastActivity = millis();
    sSrv.send(code, "application/json", body.length() ? body : "{}");
}

static void sendErr(const char* err, int code = 400)
{
    String j = "{";
    jsonBool(j, "ok", false);
    jsonStr(j, "err", err);
    j += "}";
    sendJson(code, j);
}

// PIN 校验: query k= 或头 X-Auth。不过就回 401(不回显任何配置)
static bool authed()
{
    String k = sSrv.hasArg("k") ? sSrv.arg("k") : sSrv.header("X-Auth");
    if (k.length() == 0) return false;
    return strcmp(k.c_str(), cfg.pin) == 0;
}

static void deny() { sendErr("bad-key", 401); }

// ==================== 端点 ====================

static void hStatus()
{
    if (!authed()) return deny();
    String j = "{";
    jsonNum(j, "fwVer", 1);
    jsonBool(j, "online", WiFi.status() == WL_CONNECTED);
    jsonStr(j, "wifi", WiFi.SSID());
    jsonStr(j, "ip", WiFi.localIP().toString());
    jsonStr(j, "campus", appCampusOnline() ? "logged-in" : "offline");
    jsonNum(j, "tokenAge", doorTokenAgeMs() / 1000);
    jsonNum(j, "tokenTtl", 3600);
    jsonBool(j, "timeSynced", doorTimeSynced());
    jsonNum(j, "seedCookies", campusSeedApplied());
    jsonNum(j, "freeHeap", (unsigned long)ESP.getFreeHeap());
    jsonStr(j, "user", cfg.user);
    jsonStr(j, "code", cfg.code);
    jsonNum(j, "cookieLen", strlen(cfg.cookie));
    jsonStr(j, "mqtt", cloudState());
    jsonStr(j, "ap", sApOn ? "on" : "off");
    DoorResult dr;
    if (doorLastResult(&dr)) {
        const char* r = (dr == DOOR_OK) ? "ok" : (dr == DOOR_ERR_NO_TOKEN) ? "no-token"
                      : (dr == DOOR_ERR_NO_TIME) ? "no-time" : (dr == DOOR_IGNORED) ? "ignored" : "rejected";
        String s = r; s += "@"; s += String(doorLastAgeMs() / 1000); s += "s";
        jsonStr(j, "lastDoor", s);
    } else {
        jsonStr(j, "lastDoor", "none");
    }
    jsonStr(j, "balance", appInfo().balance);
    j += "}";
    sendJson(200, j);
}

static void hSave()
{
    if (!authed()) return deny();

    const int nArgs = sizeof(gArgs) / sizeof(gArgs[0]);

    // 第一遍只校验长度: 有一条超限就整体拒绝, 免得出现"一半写进去、又报 413"的矛盾状态
    bool tooLong = false;
    const char* badField = "";
    int badMax = 0;
    for (int i = 0; i < nArgs; i++) {
        if (!sSrv.hasArg(gArgs[i].http)) continue;
        size_t len = sSrv.arg(gArgs[i].http).length();
        int max = settingsFieldMax(gArgs[i].field);
        if ((int)len > max) {
            tooLong = true; badField = gArgs[i].http; badMax = max;
            if (strcmp(gArgs[i].field, "pin") != 0) break;   // pin 的 too-short 走第二遍的 per-field 错误
        }
    }
    if (tooLong) {
        String j = "{";
        jsonBool(j, "ok", false);
        jsonStr(j, "err", "too-large");
        jsonStr(j, "field", badField);
        jsonNum(j, "max", badMax);
        j += "}";
        return sendJson(413, j);
    }

    // 第二遍写入
    String saved = "[", errs = "{";            // saved 是数组, errors 是对象(与文档一致)
    bool savedAny = false, errAny = false, pinChanged = false;
    for (int i = 0; i < nArgs; i++) {
        if (!sSrv.hasArg(gArgs[i].http)) continue;
        String v = sSrv.arg(gArgs[i].http);
        // 热点口令就是 PIN: 值真的变了才需要重开热点(值没变 settingsSet 会直接返回成功)
        bool thisIsNewPin = (strcmp(gArgs[i].field, "pin") == 0) && sApOn &&
                            strcmp(v.c_str(), cfg.pin) != 0;
        const char* err = nullptr;
        if (settingsSet(gArgs[i].field, v.c_str(), &err)) {
            if (thisIsNewPin) pinChanged = true;
            if (savedAny) saved += ',';
            saved += '"'; saved += gArgs[i].http; saved += '"';
            savedAny = true;
        } else {
            if (errAny) errs += ',';
            errs += '"'; errs += gArgs[i].http; errs += "\":\""; errs += err ? err : "failed"; errs += '"';
            errAny = true;
        }
    }
    // 提交里出现了不认识的名字 → 也报进 errors, 免得 App 以为写进去了
    for (unsigned int i = 0; i < sSrv.args(); i++) {
        String name = sSrv.argName(i);
        if (name == "k") continue;
        bool known = false;
        for (int j = 0; j < nArgs; j++)
            if (name == gArgs[j].http) { known = true; break; }
        if (known) continue;
        if (errAny) errs += ',';
        errs += '"'; errs += name; errs += "\":\"unknown-field\"";
        errAny = true;
    }
    saved += "]"; errs += "}";

    if (!savedAny && !errAny) return sendErr("no-fields");   // 包含"只提交了未知字段"的情况

    String j = "{";
    jsonBool(j, "ok", !errAny);
    if (saved.length() > 2) jsonRaw(j, "saved", saved);
    if (errs.length() > 2)  jsonRaw(j, "errors", errs);
    jsonBool(j, "needReboot", false);
    j += "}";
    sApRestart = pinChanged;          // 响应发完再重开热点(见 provHandleClient)
    sendJson(200, j);
}

static void hApply()
{
    if (!authed()) return deny();
    if (millis() - sLastApply < PROV_APPLY_GAP) {
        String j = "{";
        jsonBool(j, "ok", false);
        jsonStr(j, "err", "busy-retry");
        jsonNum(j, "afterMs", PROV_APPLY_GAP - (millis() - sLastApply));
        j += "}";
        return sendJson(429, j);
    }
    sLastApply = millis();

    ApplySteps st;
    campusUpSteps(st);

    String steps = "{";
    jsonBool(steps, "wifi", st.wifi);
    jsonBool(steps, "campus", st.campus);
    jsonBool(steps, "time", st.time);
    jsonBool(steps, "token", st.token);
    steps += "}";
    String detail = "{";
    jsonStr(detail, "why", st.why);
    detail += "}";

    String j = "{";
    jsonBool(j, "ok", st.wifi && st.campus && st.token);
    jsonRaw(j, "steps", steps);
    jsonRaw(j, "detail", detail);
    jsonNum(j, "freeHeap", (unsigned long)ESP.getFreeHeap());
    j += "}";
    sendJson(200, j);
}

static void hOpen()
{
    if (!authed()) return deny();
    if (millis() - sLastOpen < PROV_OPEN_GAP) {
        String j = "{";
        jsonBool(j, "ok", false);
        jsonStr(j, "err", "busy-retry");
        jsonNum(j, "afterMs", PROV_OPEN_GAP - (millis() - sLastOpen));
        j += "}";
        return sendJson(429, j);
    }
    sLastOpen = millis();

    DoorResult r = requestOpenDoor("配网");
    String j = "{";
    jsonStr(j, "result", (r == DOOR_OK) ? "ok" : (r == DOOR_ERR_NO_TOKEN) ? "no_token"
                          : (r == DOOR_ERR_NO_TIME) ? "no_time" : (r == DOOR_IGNORED) ? "ignored"
                          : "rejected");
    jsonStr(j, "source", "prov");
    // ignored = 根本没发请求, 别把上一次的响应混进来误导排查
    jsonStr(j, "resp", r == DOOR_IGNORED ? String() : String(doorLastResp()));
    j += "}";
    sendJson(200, j);
}

// 单页表单: 手工排障用(App 没写好之前也能全流程操作)。故意做小, 不引前端库。
static const char PAGE[] PROGMEM = R"rawliteral(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<meta charset=utf-8><title>DLUT Door</title><style>body{font:15px/1.6 system-ui;margin:18px;max-width:560px}
input,button{font:inherit;padding:7px;width:100%;box-sizing:border-box;margin:4px 0}
label{display:block;margin-top:10px}pre{background:#f4f4f4;padding:8px;overflow:auto}</style>
<h3>DLUT 门禁 · 本地配置</h3>
<label>PIN<input id=k placeholder="设备口令"></label>
<label>WiFi SSID<input id=ssid></label><label>WiFi 密码<input id=wifipswd></label>
<label>学号<input id=user></label><label>认证密码<input id=pass type=password></label>
<label>门禁设备编号<input id=code></label>
<label>Cookie(整条浏览器 Cookie 头)<textarea id=cookie rows=4 style="width:100%;font:12px monospace"></textarea></label>
<label>巴法私钥<input id=bemfakey></label><label>控制主题<input id=doortopic></label>
<label>配置主题<input id=cfgtopic></label><label>回执主题<input id=acktopic></label>
<button onclick="post('save')">1 保存</button>
<button onclick="post('apply')">2 登录并取 token</button>
<button onclick="post('open')">3 试开门</button>
<button onclick="get()">状态</button>
<pre id=o></pre>
<script>
const F=['ssid','wifipswd','user','pass','code','cookie','bemfakey','doortopic','cfgtopic','acktopic'];
function body(){let b='';for(const f of F){const e=document.getElementById(f);if(e.value!=='')b+=(b?'&':'')+f+'='+encodeURIComponent(e.value)}return b}
async function post(p){const k=document.getElementById('k').value;
 o.textContent='… '+p;o.style.background='#eee';
 try{const r=await fetch('/'+p+(p=='save'?'':'?k='+encodeURIComponent(k)),{method:'POST',
  headers:p=='save'?{'Content-Type':'application/x-www-form-urlencoded','X-Auth':k}:{'X-Auth':k},
  body:p=='save'?body():null});o.textContent=await r.text()}catch(e){o.textContent='失败: '+e}}
async function get(){const k=document.getElementById('k').value;
 try{const r=await fetch('/status?k='+encodeURIComponent(k));const j=JSON.parse(await r.text());
  o.textContent=JSON.stringify(j,null,1);document.getElementById('ssid').value=j.wifi||''}
 catch(e){o.textContent='失败: '+e}}
</script>)rawliteral";

static void hPage()
{
    sLastActivity = millis();
    sSrv.send_P(200, "text/html; charset=utf-8", PAGE);
}

// ==================== 热点生命周期 ====================

void provBegin()
{
    sSrv.on("/", HTTP_GET, hPage);
    sSrv.on("/status", HTTP_GET, hStatus);
    sSrv.on("/save", HTTP_POST, hSave);
    sSrv.on("/apply", HTTP_POST, hApply);
    sSrv.on("/open", HTTP_POST, hOpen);
    sSrv.onNotFound([]() { sendErr("not-found", 404); });
}

void provStartAp(const char* why)
{
    if (sApOn) return;
    uint8_t mac[6] = { 0 };
    WiFi.macAddress(mac);
    char suffix[8], ap[32];
    snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
    snprintf(ap, sizeof(ap), "DLUT-Door-%s", suffix);

    // 热点口令直接用 PIN: 少一个要记的密码, 也逼着用户首次配网就改掉默认 PIN。
    // 信道跟着 STA 走 —— AP+STA 并发时 ESP-IDF 要求两者同信道, 否则要么连接失败
    // 要么 STA 被踢; STA 还没连上时先固定 1 信道。
    int chan = (WiFi.status() == WL_CONNECTED) ? (int)WiFi.channel() : 0;
    WiFi.mode(WIFI_AP_STA);                      // AP+STA 并发: 配网的同时校园网会话不断
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));   // 2.0.11 的 softAPIP() 只读, 要这样设
    WiFi.softAP(ap, cfg.pin, chan);
    sSrv.begin();
    sApOn = true;
    sLastActivity = millis();
    Serial.printf("\n[配网] 热点已开: %s (口令同 PIN)  http://192.168.4.1/  —— %s\n", ap, why);
}

void provStopAp()
{
    if (!sApOn) return;
    sSrv.close();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    sApOn = false;
    Serial.println("[配网] 热点已关闭");
}

bool provApActive() { return sApOn; }

void provHandleClient()
{
    if (!sApOn) return;
    sSrv.handleClient();

    if (sApRestart) {          // PIN 刚改过: 热点口令跟着换, 手机要用新口令重连
        sApRestart = false;
        provStopAp();
        provStartAp("PIN 已更新");
        return;
    }
    if (millis() - sLastActivity > PROV_AP_IDLE_MS) {
        Serial.println("[配网] 10 分钟无操作, 关热点");
        provStopAp();
    }
}
