// ====================================================================
// 大连理工门禁开门 (menjin.dlut.edu.cn / shfb)
// 登录链复用校园网固件的 CAS 会话(campusnet.cpp 的分作用域 cookie jar):
//   已持 CASTGC → GET sso/cas/login?service=<门禁首页> → 302 门禁首页?ticket=ST-...
//   → GET 该票据地址(必要时再 GET 首页) → Set-Cookie: shfb-token
// 开门: POST /cser/device/info/command/sendRoomBatch
//   AUTH-SIGN = md5(毫秒时间戳 + "#" + 盐 + "#" + 4位随机串) + 随机串
//   AUTH-TIMESTAMP = 毫秒时间戳(与签名同一个)
// 时间戳取自苏宁时间接口: 校园网未认证时任何外网 HTTP 都会被网关劫持,
// 故必须等 campusLogin 成功后再校时, 也不用 SNTP(UDP 123 常被挡)。
// ====================================================================

#include "door.h"
#include "campusnet.h"
#include "config.h"   // DOOR_PROJECT_CD / DOOR_SIGN_SALT (编译期)
#include "settings.h" // cfg.user(学号=personId) / cfg.code(门禁设备编号)

#include <HTTPClient.h>
#include <MD5Builder.h>

// ==================== 门禁端点 ====================
static const char* MENJIN_HOST    = "menjin.dlut.edu.cn";
static const char* MENJIN_SERVICE = "http://menjin.dlut.edu.cn/cser/static/menjin/index.html";
static const char* MENJIN_CMD_URL = "http://menjin.dlut.edu.cn/cser/device/info/command/sendRoomBatch";
static const char* TIME_API_URL   = "http://f.m.suning.com/api/ct.do";

// 设备编号 / personId(学号) 走 settings(NVS, 出厂默认在 config.h)；
// 项目码与签名盐是全校固定/前端硬编码常量, 直接用 config.h 的宏。

#define DOOR_TOKEN_TTL 1800000UL   // token 每 30 分钟主动刷新一次(另有开门失败后即时刷新)

static String gToken;
static unsigned long gTokenAt = 0;

// 最近一次开门的结果与服务端响应(截断), 供本地 HTTP /open 与云回执使用
static char gLastResp[200] = { 0 };
static DoorResult gLastDoor = DOOR_ERR_REJECTED;
static unsigned long gLastDoorAt = 0;

static unsigned long long gTimeMs = 0;   // 接口返回的毫秒时间戳
static unsigned long gTimeAt = 0;        // 取值时的 millis
static bool gTimeSynced = false;

// ==================== 校时 ====================

bool doorSyncTime(int maxRetries)
{
    Serial.println("\n[校时] 获取网络时间...");
    HTTPClient http;
    http.setTimeout(10000);

    for (int i = 0; i < maxRetries; i++) {
        http.begin(TIME_API_URL);
        int code = http.GET();
        if (code == 200) {
            String payload = http.getString();
            http.end();
            // {"api":"time","code":"1","currentTime":1774456155486,"msg":""}
            int idx = payload.indexOf("\"currentTime\":");
            if (idx != -1) {
                int s = idx + 14;
                int e = payload.indexOf(',', s);
                if (e == -1) e = payload.indexOf('}', s);
                gTimeMs = strtoull(payload.substring(s, e).c_str(), NULL, 10);
                gTimeAt = millis();
                gTimeSynced = true;
                Serial.printf("[校时] 时间戳: %llu\n", gTimeMs);
                return true;
            }
            Serial.println("[校时] 响应中无 currentTime(可能被网关劫持): " + payload.substring(0, 120));
        } else {
            Serial.printf("[校时] HTTP %d (第 %d 次)\n", code, i + 1);
            http.end();
        }
        for (int w = 0; w < 100 * (i + 1); w++) { delay(10); campusYield(); }   // 退避期间也让云端活着
    }
    return false;
}

// 当前毫秒时间戳(接口时间 + 本地运行时长)
static String doorTimestamp()
{
    if (!gTimeSynced) return "0";
    return String(gTimeMs + (millis() - gTimeAt));
}

// ==================== 签名 ====================

static String randomString(int length)
{
    const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    String out;
    for (int i = 0; i < length; i++)
        out += chars[esp_random() % (sizeof(chars) - 1)];
    return out;
}

static String md5Hash(const String& input)
{
    MD5Builder md5;
    md5.begin();
    md5.add(input);
    md5.calculate();
    return md5.toString();
}

// ==================== token ====================

// 响应的首部部分(调试打印用, 不含正文)
static String respHead(const String& res)
{
    int e = res.indexOf("\r\n\r\n");
    return (e > 0) ? res.substring(0, e) : res;
}

bool doorAcquireToken()
{
    Serial.println("\n[门禁] 换取 shfb-token...");
    String ticket;
    String token;

    if (campusCasTicket(MENJIN_SERVICE, ticket)) {
        Serial.println("    票据: " + ticket);
        // 票据落地: 服务端在此响应中下发 shfb-token
        String res = campusHttp(ticket, "GET", "", "", "");
        campusSaveCookies(res);
        token = campusCookie(MENJIN_HOST, "shfb-token");
        if (token.length() == 0) {
            // 兜底: 再显式访问一次门禁首页(浏览器抓包两步都会做)
            res = String();
            res = campusHttp(MENJIN_SERVICE, "GET", "", "", "");
            campusSaveCookies(res);
            token = campusCookie(MENJIN_HOST, "shfb-token");
        }
        if (token.length() == 0) {
            Serial.println("[门禁] 响应中无 shfb-token, 首部:");
            Serial.println(respHead(res));
        }
    } else {
        Serial.println("[门禁] CAS 换票失败(统一身份认证会话不可用)");
    }

    // 拿不到 token(一般是 CAS 弹二次认证, 表单过不去): 用 config.h COOKIE_INPUT 粘的那份
    if (token.length() == 0) {
        String seeded = campusSeedCookie("shfb-token");
        if (seeded.length() && seeded != gToken) {
            Serial.println("[门禁] 改用 COOKIE_INPUT 里的 shfb-token");
            token = seeded;
        }
    }
    if (token.length() == 0) return false;

    gToken = token;
    gTokenAt = millis();
    Serial.println("[门禁] token: " + gToken.substring(0, 24) + "...");
    return true;
}

bool doorTokenExpired()
{
    if (gToken.length() == 0) return true;
    return millis() - gTokenAt >= DOOR_TOKEN_TTL;
}

// ==================== 开门 ====================

// 响应 {"success":true,...} —— 容忍冒号两侧空格
static bool jsonSuccess(const String& body)
{
    int p = body.indexOf("\"success\"");
    if (p == -1) return false;
    int q = body.indexOf(':', p);
    if (q == -1) return false;
    return body.substring(q, q + 10).indexOf("true") != -1;
}

static bool doorSendCommand()
{
    String ts = doorTimestamp();
    String rnd = randomString(4);
    String sign = md5Hash(ts + "#" + DOOR_SIGN_SALT + "#" + rnd) + rnd;

    // conditions 为 JSON, 整串转义后放进表单体; personId 用学号(cfg.user)
    String conditions = String("{\"personId\":\"") + cfg.user + "\",\"delStatus\":\"0\"}";
    String body = String("commandCode=OPEN") +
                  "&conditions=" + campusUrlEncode(conditions) +
                  "&deviceCode=" + cfg.code +
                  "&isCommon=yes" +
                  "&pageSize=-1" +
                  "&projectCd=" + DOOR_PROJECT_CD +
                  "&token=" + campusUrlEncode(gToken);

    Serial.println("\n[开门] POST sendRoomBatch");
    Serial.println("    ts=" + ts + " sign=" + sign);

    String res = campusHttp(MENJIN_CMD_URL, "POST", body, "application/x-www-form-urlencoded",
                            "AUTH-SIGN: " + sign + "\r\n" +
                            "AUTH-TIMESTAMP: " + ts + "\r\n" +
                            "Origin: http://" + String(MENJIN_HOST) + "\r\n" +
                            "Referer: " + String(MENJIN_SERVICE) + "\r\n");
    String respBody = campusBody(res);
    // 留一份给本地 HTTP /open 与云回执回显(排障靠它)
    strlcpy(gLastResp, (respBody.length() ? respBody : respHead(res)).c_str(), sizeof(gLastResp));
    Serial.println("    响应: " + String(gLastResp));
    return jsonSuccess(respBody);
}

static DoorResult remember(DoorResult r)
{
    gLastDoor = r;
    gLastDoorAt = millis();
    return r;
}

DoorResult doorOpen()
{
    if (!gTimeSynced && !doorSyncTime(3)) {
        Serial.println("[开门] 校时失败, 无法生成签名");
        return remember(DOOR_ERR_NO_TIME);
    }
    if (doorTokenExpired() && !doorAcquireToken())
        return remember(DOOR_ERR_NO_TOKEN);

    if (doorSendCommand()) return remember(DOOR_OK);

    // 失败多为 token 失效 → 重取一次再试
    Serial.println("[开门] 首次失败, 重新取 token 后重试...");
    gToken = "";
    if (!doorAcquireToken()) return remember(DOOR_ERR_NO_TOKEN);
    return remember(doorSendCommand() ? DOOR_OK : DOOR_ERR_REJECTED);
}

// ==================== 状态查询(供 /status 与云回执) ====================

bool doorTimeSynced() { return gTimeSynced; }

unsigned long doorTokenAgeMs()
{
    return gToken.length() ? (millis() - gTokenAt) : 0;
}

unsigned long doorTokenTtlMs() { return DOOR_TOKEN_TTL; }

const char* doorLastResp() { return gLastResp; }

bool doorLastResult(DoorResult* out)
{
    if (out) *out = gLastDoor;
    return gLastDoorAt != 0;
}

unsigned long doorLastAgeMs()
{
    return gLastDoorAt ? (millis() - gLastDoorAt) : 0;
}
