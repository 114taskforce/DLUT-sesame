// ====================================================================
// 运行时配置的实现: RAM 里一份 Settings, NVS 里逐键存
//   - settingsLoad() 读 NVS 覆盖出厂默认(config.h), 没读到的键保持默认
//   - settingsSet()  写 RAM 并立刻落 NVS(掉电即生效, 不做批量提交)
// 键名长度受 Preferences 限制(≤15 字符), 所以用短名。
// ====================================================================

#include "settings.h"
#include "config.h"

#include <Preferences.h>

Settings cfg;

static Preferences sNvs;
static bool sNvsReady = false;

// 字段表: 名字 / NVS 键 / 成员偏移 / 缓冲区大小 / 允许的最大字符数 / 是否敏感
struct Field {
    const char* name;
    const char* nvsKey;
    size_t offset;
    size_t bufSize;
    uint16_t maxLen;
    bool secret;
};

// 注意别把宏叫 F —— 会和 Arduino 的 F() 字符串宏撞名
#define FLD(field, key, max) { #field, key, offsetof(Settings, field), sizeof(((Settings*)0)->field), max, false }
#define SEC(field, key, max) { #field, key, offsetof(Settings, field), sizeof(((Settings*)0)->field), max, true  }

static const Field gFields[] = {
    FLD(ssid,      "ssid",     32),
    SEC(wifipswd,  "wifipswd", 64),
    FLD(user,      "user",     20),
    SEC(pass,      "pass",     36),
    FLD(code,      "code",     20),
    SEC(cookie,    "cookie",   SET_COOKIE_MAX),
    SEC(pin,       "pin",      16),
    SEC(bemfakey,  "bemfakey", 32),
    FLD(doorTopic, "doortopic",64),
    FLD(cfgTopic,  "cfgtopic", 64),
    FLD(ackTopic,  "acktopic", 64),
    FLD(portalUrl, "portal",   SET_URL_MAX),
};
static const int gFieldCount = sizeof(gFields) / sizeof(gFields[0]);

static const Field* findField(const char* key)
{
    if (!key) return nullptr;
    for (int i = 0; i < gFieldCount; i++)
        if (strcmp(gFields[i].name, key) == 0) return &gFields[i];
    return nullptr;
}

static char* slot(const Field* f)
{
    return (char*)&cfg + f->offset;
}

// 出厂默认(config.h)
static void fillDefaults()
{
    strlcpy(cfg.ssid,      WIFI_SSID,        sizeof(cfg.ssid));
    strlcpy(cfg.wifipswd,  WIFI_PSWD,        sizeof(cfg.wifipswd));
    strlcpy(cfg.user,      CAMPUS_USER,      sizeof(cfg.user));
    strlcpy(cfg.pass,      CAMPUS_PASS,      sizeof(cfg.pass));
    strlcpy(cfg.code,      DOOR_DEVICE_CODE, sizeof(cfg.code));
    strlcpy(cfg.cookie,    COOKIE_INPUT,     sizeof(cfg.cookie));
    strlcpy(cfg.pin,       DEVICE_PIN,       sizeof(cfg.pin));
    strlcpy(cfg.bemfakey,  BEMFA_KEY,        sizeof(cfg.bemfakey));
    strlcpy(cfg.doorTopic, DOOR_TOPIC,       sizeof(cfg.doorTopic));
    strlcpy(cfg.cfgTopic,  CFG_TOPIC,        sizeof(cfg.cfgTopic));
    strlcpy(cfg.ackTopic,  ACK_TOPIC,        sizeof(cfg.ackTopic));
    cfg.portalUrl[0] = '\0';
}

void settingsLoad()
{
    fillDefaults();
    sNvsReady = sNvs.begin("campus", false);   // 读写模式, campusnet 也用它存 portal
    if (!sNvsReady) {
        Serial.println("[配置] NVS 打开失败, 只用 config.h 出厂默认");
        return;
    }
    int loaded = 0;
    for (int i = 0; i < gFieldCount; i++) {
        if (!sNvs.isKey(gFields[i].nvsKey)) continue;
        String v = sNvs.getString(gFields[i].nvsKey, "");
        // portal 是 campusnet 的内部键, 不算"用户配置", 不计入 loaded
        if (v.length() == 0) continue;
        strlcpy(slot(&gFields[i]), v.c_str(), gFields[i].bufSize);
        loaded++;
    }
    Serial.printf("[配置] NVS 载入 %d 项, 其余用出厂默认\n", loaded);
}

bool settingsSet(const char* key, const char* value, const char** err)
{
    const Field* f = findField(key);
    if (!f) { if (err) *err = "unknown-field"; return false; }
    if (!value) { if (err) *err = "empty"; return false; }
    size_t len = strlen(value);
    if (len > f->maxLen) { if (err) *err = "too-long"; return false; }
    // PIN 同时用作配网热点的 WPA2 口令, 短于 8 位热点会开成开放网络 → 直接拒绝
    if (strcmp(f->name, "pin") == 0 && len < 8) { if (err) *err = "too-short"; return false; }

    if (strcmp(slot(f), value) == 0) {     // 没变就不写 flash, 省 NVS 磨损
        if (err) *err = nullptr;
        return true;
    }
    strlcpy(slot(f), value, f->bufSize);

    if (sNvsReady) {
        if (!sNvs.putString(f->nvsKey, value)) {
            if (err) *err = "nvs";
            Serial.printf("[配置] %s 写 NVS 失败(仅本次运行生效)\n", f->name);
            return false;
        }
    }
    // 密码/cookie 只打长度, 别把内容写进串口日志
    if (f->secret) Serial.printf("[配置] %s 已更新 (%u 字节)\n", f->name, (unsigned)strlen(value));
    else Serial.printf("[配置] %s = %s\n", f->name, value);
    if (err) *err = nullptr;
    return true;
}

int settingsFieldMax(const char* key)
{
    const Field* f = findField(key);
    return f ? (int)f->maxLen : 0;
}

bool settingsHasWifi()
{
    return cfg.ssid[0] != '\0';
}

void settingsDump()
{
    Serial.println("=========== 当前配置 ===========");
    for (int i = 0; i < gFieldCount; i++) {
        const char* v = slot(&gFields[i]);
        if (gFields[i].secret)
            Serial.printf("  %-9s: (%u 字节)\n", gFields[i].name, (unsigned)strlen(v));
        else
            Serial.printf("  %-9s: %s\n", gFields[i].name, v[0] ? v : "(空)");
    }
}
