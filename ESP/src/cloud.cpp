// ====================================================================
// 巴法云 MQTT 通道实现(手写最小 3.1.1 子集, 零依赖)。
// 协议细节见 cloud.h 顶部注释与 docs/app-api.md 的 2。
// ====================================================================

#include "cloud.h"
#include "app.h"
#include "config.h"
#include "settings.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <string.h>

#define BEMFA_HOST    "mqtt.bemfa.com"         // 控制台给的地址(bemfa.com:9501 同一集群, 都通)
#define BEMFA_PORT    9501                     // 实测: 1883 关闭; CONNACK rc=0, clientId=私钥
#define KEEPALIVE_S   60                       // 服务器 65 秒无心跳判掉线
#define PING_EVERY_MS 50000UL
#define RECONNECT_FAST_MS  5000UL              // 首次/偶发失败
#define RECONNECT_SLOW_MS  60000UL             // 连续失败后放慢, 别刷爆日志
#define CONNACK_WAIT_MS    10000UL             // CONNECT 后多久没回包就重连
#define CFG_FRAME_GAP_MS  20000UL              // 分帧装配超时 → 丢弃并回执
#define MQTT_IN_MAX   256                      // 入站包体上限
#define MQTT_OUT_MAX  256                      // 出站包体上限(topic+msg)
#define CFG_B64_MAX   1024                     // 装配后的 base64 总长上限
#define CFG_JSON_MAX  768                      // 解码后的 JSON 上限
#define CFG_CHUNK     96                       // 每帧 base64 字符数(须与 App 一致)
#define CFG_MAX_FRAMES 16
#define ACK_MAX       96
#define TOPIC_MAX     66                       // 巴法主题名 ≤64

enum State { ST_OFF, ST_DOWN, ST_CONNECTING, ST_CONNECTED };
static State sState = ST_OFF;
static WiFiClient sSock;
static unsigned long sNextTry = 0;
static unsigned long sLastPing = 0;
static int sFailStreak = 0;
static bool sApplyPending = false;

// ---------- 入站逐字节解析 ----------
enum RdStep { RD_HDR, RD_LEN, RD_BODY };
static RdStep sRd = RD_HDR;
static uint8_t sRdHdr = 0;
static uint32_t sRdRem = 0, sRdGot = 0;
static uint8_t sRdBuf[MQTT_IN_MAX];

// ---------- 长流程期间到达的消息先进单槽队列 ----------
static char sQueued[ACK_MAX];
static bool sQueuedIsCfg = false;
static bool sHasQueued = false;
static bool sInPump = false;

// ---------- 配置分帧装配 ----------
static char sCfgB64[CFG_B64_MAX];
static uint32_t sCfgHave = 0;                  // 已收帧位图
static uint16_t sCfgLen[CFG_MAX_FRAMES];       // 每帧实际字符数(末帧可短)
static uint8_t sCfgTotal = 0;
static char sCfgMsgId[10] = "";
static unsigned long sCfgFirstAt = 0;

static const char* sStateName = "disabled";    // 供 /status 回显

static void setState(State s)
{
    if (s == sState) return;
    sState = s;
    sStateName = (s == ST_OFF) ? "disabled" : (s == ST_DOWN) ? "down"
               : (s == ST_CONNECTING) ? "connecting" : "connected";
}

// ==================== base64url / crc8 / 极简 JSON ====================

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

// base64url(允许无填充) → 原始字节; 返回字节数, 非法/溢出返回 -1
static int b64urlDecode(const char* in, size_t inLen, uint8_t* out, size_t outCap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < inLen; i++) {
        if (in[i] == '=') continue;
        int v = b64val(in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= outCap) return -1;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return (int)n;
}

static uint8_t crc8(const uint8_t* d, size_t n)   // 多项式 0x07, 初值 0
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

// 从 {"k":"v",...} 取值(只需处理自己生成的 JSON; 还原 \" \\ \n)
static bool jsonVal(const char* js, int len, const char* key, char* out, size_t cap)
{
    size_t kl = strlen(key);
    for (int i = 0; i + (int)kl + 3 < len; i++) {
        if (js[i] != '"' || js[i + 1 + kl] != '"') continue;
        if (memcmp(js + i + 1, key, kl) != 0) continue;
        int j = i + 2 + kl;
        while (j < len && js[j] != ':') j++;
        if (j >= len) return false;
        j++;
        while (j < len && (js[j] == ' ' || js[j] == '\t')) j++;
        if (j >= len || js[j] != '"') continue;         // 非字符串值(null/数字)跳过
        j++;
        size_t n = 0;
        while (j < len && js[j] != '"' && n + 1 < cap) {
            char c = js[j];
            if (c == '\\' && j + 1 < len) {
                j++;
                char e = js[j];
                out[n++] = (e == 'n') ? '\n' : (e == 'r') ? '\r' : (e == 't') ? '\t' : e;
            } else out[n++] = c;
            j++;
        }
        out[n] = '\0';
        return n > 0;
    }
    return false;
}

// ==================== MQTT 报文 ====================

static size_t mqttRemLen(uint32_t n, uint8_t* out)
{
    size_t i = 0;
    do {
        uint8_t b = (uint8_t)(n % 128);
        n /= 128;
        if (n) b |= 0x80;
        out[i++] = b;
    } while (n && i < 4);
    return i;
}

static bool mqttSend(uint8_t hdr, const uint8_t* body, size_t len)
{
    uint8_t h[5];
    h[0] = hdr;
    size_t hl = 1 + mqttRemLen((uint32_t)len, h + 1);
    if (!sSock.connected()) return false;
    if (!sSock.write(h, hl)) return false;
    return len == 0 || sSock.write(body, (uint32_t)len) == len;
}

static size_t putStr(uint8_t* p, const char* s, size_t used, size_t cap)   // 失败原样返回 used
{
    size_t n = strlen(s);
    if (used + n + 2 > cap) return (size_t)-1;
    p[used] = (uint8_t)(n >> 8);
    p[used + 1] = (uint8_t)(n & 0xFF);
    memcpy(p + used + 2, s, n);
    return used + n + 2;
}

// SUBSCRIBE 的 packet id → 主题名: 巴法一个 SUBACK 回一个 id, 不记就不知道是哪个主题被拒
struct PendingSub { uint16_t id; char topic[TOPIC_MAX + 2]; };
static PendingSub sPendSub[4];
static int sPendN = 0;
static uint16_t sPktId = 1;

static uint16_t nextPktId()
{
    uint16_t id = sPktId++;
    if (sPktId == 0) sPktId = 1;                  // 0 不是合法 packet id
    return id;
}

static void rememberSub(uint16_t id, const char* topic)
{
    PendingSub& p = sPendSub[sPendN++ % 4];
    p.id = id;
    strlcpy(p.topic, topic, sizeof(p.topic));
}

static const char* lookupSub(uint16_t id)
{
    for (int i = 0; i < 4; i++)
        if (sPendSub[i].id == id && sPendSub[i].topic[0]) return sPendSub[i].topic;
    return "(未知主题)";
}

static bool sendConnect()
{
    uint8_t b[96];
    size_t n = 0;
    if ((n = putStr(b, "MQTT", 0, sizeof(b))) == (size_t)-1) return false;
    b[n++] = 4;                                 // protocol level 4 = 3.1.1
    b[n++] = 0x02;                              // flags: 仅 clean session(私钥当 clientId, 不带 user/pass)
    b[n++] = (KEEPALIVE_S >> 8) & 0xFF;
    b[n++] = KEEPALIVE_S & 0xFF;
    if ((n = putStr(b, cfg.bemfakey, n, sizeof(b))) == (size_t)-1) return false;
    return mqttSend(0x10, b, n);
}

static bool sendSubscribe(const char* topic)
{
    uint16_t id = nextPktId();
    rememberSub(id, topic);
    uint8_t b[TOPIC_MAX + 16];
    size_t n = 0;
    b[n++] = (uint8_t)(id >> 8);                 // packet id: 每次订阅都不同, SUBACK 才对得上是哪个主题
    b[n++] = (uint8_t)(id & 0xFF);
    if ((n = putStr(b, topic, n, sizeof(b))) == (size_t)-1) return false;
    b[n++] = 0;                                  // QoS0
    return mqttSend(0x82, b, n);
}

static void sendPublish(const char* topic, const char* msg)
{
    if (sState != ST_CONNECTED || !topic[0]) return;
    static uint8_t out[MQTT_OUT_MAX];
    size_t n = putStr(out, topic, 0, sizeof(out));
    if (n == (size_t)-1) return;
    size_t ml = strlen(msg);
    if (n + ml > sizeof(out)) return;
    memcpy(out + n, msg, ml);
    mqttSend(0x30, out, n + ml);
    sLastPing = millis();                         // 发消息顺带当心跳
}

static void sendPing()
{
    mqttSend(0xC0, nullptr, 0);
}

static void ack(const char* text)
{
    if (cfg.ackTopic[0]) sendPublish(cfg.ackTopic, text);
}

// ==================== 业务 ====================

static void dropCfgAssembly()
{
    sCfgHave = 0;
    sCfgTotal = 0;
    sCfgMsgId[0] = '\0';
}

// c,<pin>,<msgId>,<total>,<seq>,<crc8>,<b64chunk>
static void handleCfgFrame(const char* body)
{
    if (body[0] != 'c' || body[1] != ',') return;
    char f[6][24];
    memset(f, 0, sizeof(f));
    int fi = 0;
    const char* chunk = nullptr;
    size_t start = 2;
    for (size_t i = 2; ; i++) {
        if (body[i] == ',' || body[i] == '\0') {
            size_t n = i - start;
            if (fi < 6) {
                if (n >= sizeof(f[0])) return;
                memcpy(f[fi], body + start, n);
            } else {
                if (n > CFG_CHUNK) return;
                chunk = body + start;
                unsigned int sq = (unsigned int)atoi(f[4]);
                size_t off = (size_t)(sq - 1) * CFG_CHUNK;
                if (sq < 1 || sq > CFG_MAX_FRAMES || off + n > sizeof(sCfgB64)) {
                    ack("cfg-err,full");
                    dropCfgAssembly();
                    return;
                }
                memcpy(sCfgB64 + off, chunk, n);        // 定长偏移 → 乱序到达也不会互相截断
                sCfgLen[sq - 1] = (uint16_t)n;
            }
            fi++;
            start = i + 1;
            if (body[i] == '\0') break;
        }
    }
    if (fi < 7 || !chunk) return;

    // PIN 不符 → 静默丢弃(不回错, 免得给人试探主题名和口令的机会)
    if (strcmp(f[1], cfg.pin) != 0) return;

    unsigned int total = (unsigned int)atoi(f[3]);
    unsigned int seq = (unsigned int)atoi(f[4]);
    if (total == 0 || total > CFG_MAX_FRAMES || seq == 0 || seq > total) return;
    uint8_t wantCrc = (uint8_t)strtol(f[5], nullptr, 16);

    if (sCfgMsgId[0] && strcmp(sCfgMsgId, f[2]) != 0) dropCfgAssembly();   // 新会话, 旧的作废
    strlcpy(sCfgMsgId, f[2], sizeof(sCfgMsgId));
    if (sCfgTotal != (uint8_t)total) { sCfgHave = 0; sCfgTotal = (uint8_t)total; }
    if (!sCfgHave) sCfgFirstAt = millis();
    sCfgHave |= (uint32_t)(1u << (seq - 1));

    for (unsigned int i = 1; i <= total; i++)
        if (!(sCfgHave & (1u << (i - 1)))) return;                      // 还没收全

    // 长度按"定长偏移 + 末帧实际长度"算, 不能靠 strlen(乱序写入时中间不会有终止符)
    size_t b64Len = (size_t)(total - 1) * CFG_CHUNK + sCfgLen[total - 1];
    for (unsigned int i = 0; i + 1 < total; i++)
        if (sCfgLen[i] != CFG_CHUNK) { ack("cfg-err,unaligned"); dropCfgAssembly(); return; }

    char msgId[10];
    strlcpy(msgId, sCfgMsgId, sizeof(msgId));
    char a[48];
    uint8_t js[CFG_JSON_MAX];
    int n = b64urlDecode(sCfgB64, b64Len, js, sizeof(js) - 1);
    if (n <= 0) { snprintf(a, sizeof(a), "cfg-err,%s,too-long", msgId); ack(a); dropCfgAssembly(); return; }
    js[n] = '\0';
    if (crc8(js, (size_t)n) != wantCrc) {
        snprintf(a, sizeof(a), "cfg-err,%s,crc", msgId);
        ack(a);
        dropCfgAssembly();
        return;
    }
    dropCfgAssembly();

    // HTTP/云 共用的字段名映射(与 docs/app-api.md 1.2 一致)
    const char* keys[]   = { "user", "pass", "code", "cookie", "ssid", "wifipswd",
                             "pin", "bemfakey", "doortopic", "cfgtopic", "acktopic" };
    const char* fields[] = { "user", "pass", "code", "cookie", "ssid", "wifipswd",
                             "pin", "bemfakey", "doorTopic", "cfgTopic", "ackTopic" };
    char saved[ACK_MAX] = "";
    bool cookieSaved = false;
    for (unsigned int i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        char v[CFG_JSON_MAX];
        if (!jsonVal((const char*)js, n, keys[i], v, sizeof(v))) continue;
        const char* err = nullptr;
        if (!settingsSet(fields[i], v, &err)) continue;
        if (strcmp(fields[i], "cookie") == 0) cookieSaved = true;
        if (saved[0]) strlcat(saved, ",", sizeof(saved));
        strlcat(saved, keys[i], sizeof(saved));
    }
    if (!saved[0]) { snprintf(a, sizeof(a), "cfg-err,%s,empty", msgId); ack(a); return; }
    // 重贴的 cookie 立即重新登记+注入种子(否则要重启才生效), 下面的整链认证就会用它
    if (cookieSaved) campusSeedCookies(cfg.cookie);
    snprintf(a, sizeof(a), "cfg-ok,%s,%s", msgId, saved);
    ack(a);
    Serial.printf("[云] 配置已写入: %s\n", saved);
    sApplyPending = true;                                    // 回主循环后跑一次整链认证
}

static void handleDoorMsg(const char* msg)
{
    if (strcmp(msg, "on") != 0) return;              // off 只回状态, 不动作
    DoorResult r = requestOpenDoor("云端");
    const char* code = (r == DOOR_OK) ? "door-ok"
                     : (r == DOOR_ERR_NO_TOKEN) ? "door-err,no-token"
                     : (r == DOOR_ERR_NO_TIME) ? "door-err,no-time"
                     : (r == DOOR_IGNORED) ? "door-err,ignored" : "door-err,rejected";
    ack(code);
    // 状态上报走 <doorTopic>/up: 只更新云端数据、不再向订阅者推送(设备自己在订阅
    // 该主题, 用裸主题发会绕回自己)。若米家显示的开关状态不更新, 改成裸 cfg.doorTopic 试。
    char st[TOPIC_MAX + 8];
    snprintf(st, sizeof(st), "%s/up", cfg.doorTopic);
    sendPublish(st, r == DOOR_OK ? "on" : "off");   // 失败回 off, 米家不会误留"已开"
}

static void queueMsg(const char* msg, bool isCfg)
{
    strlcpy(sQueued, msg, sizeof(sQueued));
    sQueuedIsCfg = isCfg;
    sHasQueued = true;
}

// body: <topic 长度2><topic>[<pktId2 若 QoS1>]<payload>
static void onPublish(const uint8_t* body, size_t len, uint8_t hdrFlags)
{
    if (len < 2) return;
    size_t tl = ((size_t)body[0] << 8) | body[1];
    size_t pos = 2 + tl;
    uint8_t qos = (uint8_t)((hdrFlags >> 1) & 3);
    uint16_t pktId = 0;
    if (qos >= 1) {
        if (pos + 2 > len) return;
        pktId = ((uint16_t)body[pos] << 8) | body[pos + 1];
        pos += 2;
    }
    size_t plen = len - pos;
    if (tl == 0 || pos > len) return;

    char topic[TOPIC_MAX + 2];
    size_t cpy = tl < sizeof(topic) - 1 ? tl : sizeof(topic) - 1;
    memcpy(topic, body + 2, cpy);
    topic[cpy] = '\0';
    char msg[ACK_MAX];
    cpy = plen < sizeof(msg) - 1 ? plen : sizeof(msg) - 1;
    memcpy(msg, body + pos, cpy);
    msg[cpy] = '\0';
    if (qos >= 1) {                              // PUBACK, 否则服务器会重发
        uint8_t pa[2] = { (uint8_t)(pktId >> 8), (uint8_t)(pktId & 0xFF) };
        mqttSend(0x40, pa, 2);
    }
    Serial.printf("[云] <- %s : %s\n", topic, msg);

    bool isCfg = cfg.cfgTopic[0] && strcmp(topic, cfg.cfgTopic) == 0;
    bool isDoor = strcmp(topic, cfg.doorTopic) == 0;
    if (!isCfg && !isDoor) return;
    if (sInPump) { queueMsg(msg, isCfg); return; }      // 长流程里只入队, 不执行
    if (isCfg) handleCfgFrame(msg);
    else handleDoorMsg(msg);
}

static void onConnack(const uint8_t* b, size_t len)
{
    if (len < 2) return;
    if (b[1] == 0) {
        setState(ST_CONNECTED);
        sFailStreak = 0;
        Serial.println("[云] 巴法云已连接");
        sendSubscribe(cfg.doorTopic);
        if (cfg.cfgTopic[0]) sendSubscribe(cfg.cfgTopic);
    } else {
        Serial.printf("[云] 连接被拒 rc=%d(私钥无效?)\n", b[1]);
        sSock.stop();
        setState(ST_DOWN);
        sNextTry = millis() + RECONNECT_SLOW_MS;
    }
}

static void onSuback(const uint8_t* b, size_t len)
{
    if (len < 3) return;                         // body = 2 字节 packet id + 每个主题 1 字节返回码
    uint16_t id = ((uint16_t)b[0] << 8) | b[1];
    for (size_t i = 2; i < len; i++) {
        const char* topic = lookupSub(id);
        if (b[i] == 0x80)
            Serial.printf("[云] 主题 %s 订阅被拒: 控制台里没建它, 或协议类型不是 MQTT。"
                          "不影响开门, 只是收不到远程配置/回执\n", topic);
        else
            Serial.printf("[云] 主题 %s 订阅成功(QoS%d)\n", topic, b[i]);
    }
}

static void feedInbound()
{
    while (sSock.available()) {
        uint8_t c = (uint8_t)sSock.read();
        if (sRd == RD_HDR) {
            sRdHdr = c;
            sRdRem = 0;
            sRdGot = 0;
            sRd = RD_LEN;
        } else if (sRd == RD_LEN) {
            sRdRem = (sRdRem << 7) | (uint32_t)(c & 0x7F);
            if (sRdRem > MQTT_IN_MAX) { sRd = RD_HDR; continue; }        // 超长包丢弃
            if (!(c & 0x80)) sRd = sRdRem ? RD_BODY : RD_HDR;
        } else {
            if (sRdGot < MQTT_IN_MAX) sRdBuf[sRdGot++] = c;
            if (--sRdRem == 0) {
                uint8_t type = sRdHdr & 0xF0;
                if (type == 0x20) onConnack(sRdBuf, sRdGot);
                else if (type == 0x90) onSuback(sRdBuf, sRdGot);
                else if (type == 0x30) onPublish(sRdBuf, sRdGot, sRdHdr);
                // 0xD0 PINGRESP / 0x40 PUBACK / 0x90 SUBACK 已覆盖, 其余忽略
                sRd = RD_HDR;
            }
        }
    }
    if (sState == ST_CONNECTED && !sSock.connected()) {
        Serial.println("[云] 连接已断开");
        sSock.stop();
        setState(ST_DOWN);
        sNextTry = millis();
    }
}

static void tryConnect()
{
    sSock.stop();
    if (!sSock.connect(BEMFA_HOST, BEMFA_PORT)) {
        sFailStreak++;
        setState(ST_DOWN);
        sNextTry = millis() + (sFailStreak > 3 ? RECONNECT_SLOW_MS : RECONNECT_FAST_MS);
        Serial.printf("[云] 连 %s:%d 失败(第 %d 次)\n", BEMFA_HOST, BEMFA_PORT, sFailStreak);
        return;
    }
    setState(ST_CONNECTING);
    sLastPing = millis();
    if (!sendConnect()) {
        sSock.stop();
        setState(ST_DOWN);
        sNextTry = millis() + RECONNECT_FAST_MS;
        return;
    }
    sNextTry = millis() + CONNACK_WAIT_MS;
}

static void run(bool allowDispatch)
{
    if (!cfg.bemfakey[0] || !cfg.doorTopic[0]) { setState(ST_OFF); return; }
    if (WiFi.status() != WL_CONNECTED) {
        if (sState != ST_DOWN) { sSock.stop(); setState(ST_DOWN); }
        sNextTry = millis() + 5000;
        return;
    }

    if (sState == ST_DOWN || sState == ST_OFF) {
        if ((long)(millis() - sNextTry) >= 0) tryConnect();
    } else if (sState == ST_CONNECTING && (long)(millis() - sNextTry) >= 0) {
        Serial.println("[云] 没收到 CONNACK, 重连");
        sSock.stop();
        setState(ST_DOWN);
        sNextTry = millis();
        return;
    }

    if (sState == ST_CONNECTED || sState == ST_CONNECTING) feedInbound();

    if (sState == ST_CONNECTED) {
        if (millis() - sLastPing >= PING_EVERY_MS) { sendPing(); sLastPing = millis(); }
        if (allowDispatch) {
            if (sHasQueued) {
                char q[ACK_MAX];
                strlcpy(q, sQueued, sizeof(q));
                bool wasCfg = sQueuedIsCfg;
                sHasQueued = false;
                if (wasCfg) handleCfgFrame(q);
                else handleDoorMsg(q);
            }
            if (sApplyPending) {
                sApplyPending = false;
                ApplySteps st;
                campusUpSteps(st);
                if (st.campus && st.token) ack("apply-ok,token");
                else {
                    char a[64];
                    snprintf(a, sizeof(a), "apply-err,%s,%s",
                             st.campus ? "token" : "campus",
                             (st.why && st.why[0]) ? st.why : "failed");
                    ack(a);
                }
            }
        }
    }

    if (sCfgHave && millis() - sCfgFirstAt > CFG_FRAME_GAP_MS) {
        char a[40];
        snprintf(a, sizeof(a), "cfg-err,%s,timeout", sCfgMsgId);
        ack(a);
        dropCfgAssembly();
    }
}

// ==================== 对外 ====================

void cloudBegin()
{
    sRd = RD_HDR;
    sNextTry = millis() + 3000;                 // 上电先给校园网 3 秒
    setState(cfg.bemfakey[0] ? ST_DOWN : ST_OFF);
    if (cfg.bemfakey[0])
        Serial.printf("[云] 目标 %s:%d 控制主题 %s 配置主题 %s\n",
                      BEMFA_HOST, BEMFA_PORT, cfg.doorTopic, cfg.cfgTopic);
}

void cloudTick() { run(true); }

void cloudPump()
{
    if (sInPump) return;                         // 防自己套自己
    sInPump = true;
    run(false);
    sInPump = false;
}

bool cloudConnected() { return sState == ST_CONNECTED; }
const char* cloudState() { return sStateName; }
