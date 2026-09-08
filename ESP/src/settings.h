#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>

// ====================================================================
// 运行时配置: 出厂默认取 config.h, 手机可经本地 HTTP(/save) 或云 MQTT 覆盖,
// 落在 NVS(Preferences 命名空间 "campus")里, 断电不丢。
//
// 读写都走 settingsSet()/settingsLoad(), 字段名与长度上限只在 gFields 表里
// 定义一次(docs/app-api.md 的 1.2 与它一一对应)。
// ====================================================================

#define SET_COOKIE_MAX   480     // 浏览器整条 Cookie 头原文
#define SET_URL_MAX      480     // eportal 认证入口地址(内部用; 带 sessionId/userMac 等一串参数)

struct Settings {
    char ssid[33];
    char wifipswd[65];
    char user[24];        // 学号, 兼作门禁 personId
    char pass[40];        // 统一身份认证密码
    char code[24];        // 门禁 deviceCode
    char cookie[SET_COOKIE_MAX + 32];   // 二次认证兜底的基础 cookie 原文
    char pin[20];         // 本地 HTTP / 云报文的口令, 同时是配网热点 WPA2 密码(故须 8~16 位)
    char bemfakey[33];    // 巴法云私钥
    char doorTopic[65];   // 控制主题(米家可见, 后三位为设备类型码)
    char cfgTopic[65];    // 配置下行主题(私有)
    char ackTopic[65];    // 回执主题(私有)
    char portalUrl[SET_URL_MAX + 16];   // 原 campusnet 的 "portal" 键
};

extern Settings cfg;

// 上电调用一次: 读 NVS, 缺项用 config.h 默认值补齐(只填进 RAM, 不写回 NVS)
void settingsLoad();

// 写字段(校验字段名与长度) → 成功则同步落 NVS。
// key 用 docs/app-api.md 里的小写名(ssid/wifipswd/user/pass/code/cookie/pin/...)。
// 返回 true=已保存; 失败时 err 指向 "unknown-field" / "too-long" / "empty" / "nvs"
bool settingsSet(const char* key, const char* value, const char** err);

// 该字段的长度上限(未知字段返回 0), 供 HTTP 413 的 max 字段用
int settingsFieldMax(const char* key);

// 是否已配过 WiFi(ssid 非空即视为已配)
bool settingsHasWifi();

// 调试打印: 密码/cookie/pin 只打长度, 不打内容
void settingsDump();

#endif
