// ====================================================================
// 校园网自动登录 + 门禁开门 (ESP32-S3)
//
//   上电: 连 WiFi → 校园网 eportal/CAS 认证 → 校时 → 换门禁 token
//   按键 GPIO0(上拉, 按下=LOW): 短按=开门 / 长按 1.5s=重新登录刷新 / 长按 8s=开配网热点
//   外部触发 GPIO4(下拉, 上升沿=米家门磁): 开门
//   后台: 每 30 分钟检测在线(掉线自动重连并重取 token), token 超 1 小时自动刷新
//   通道: 本地配网 HTTP(prov.cpp) + 云 MQTT(cloud.cpp, 待接入) —— 都调本文件的
//         requestOpenDoor()/campusUpSteps(), 不各自实现一份开门逻辑
//
//   开门依赖校园网会话: 门禁与校园网门户同走 sso.dlut.edu.cn 统一身份认证,
//   登录后的 CASTGC 直接给门禁签 CAS 票据换 shfb-token, 全程只提交一次账号密码。
//
//   LED: 蓝=登录中, 绿闪=成功, 黄=token 重试中, 红=失败, 紫闪=配网热点已开
//   登录成功还打印: 余额 / 剩余流量 / 在线设备
// ====================================================================

#include <WiFi.h>
#include "app.h"
#include "campusnet.h"
#include "cloud.h"
#include "config.h"
#include "door.h"
#include "prov.h"
#include "settings.h"
#include "Freenove_WS2812_Lib_for_ESP32.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern TaskHandle_t loopTaskHandle;   // 框架 loop 任务句柄(查栈余量用)

// ==================== 硬件 ====================
#define TRIGGER_BUTTON_PIN 0      // 按键: 短按开门 / 长按重新登录 / 长按 8s 配网
#define MIJIA_PIN          4      // 米家门磁等外部触发: 上升沿开门
#define LEDS_COUNT  3
#define LEDS_PIN    48
#define CHANNEL     0

// ==================== 周期与时长 ====================
#define CHECK_INTERVAL   1800000UL   // 30 分钟在线检测
#define LONG_PRESS_MS    1500        // 按键长按阈值(刷新会话)
#define PROV_PRESS_MS    8000        // 按键超长按阈值(开配网热点)
#define PRESS_BOUNCE_MS  30          // 释放沿最短有效按压时长(滤机械抖动)
#define DOOR_MIN_INTERVAL 1500       // 两次开门请求的最小间隔, 任何触发源共用
#define MIJIA_GAP         2400       // 门磁两次触发的最小间隔(米家模块会连发)
#define TOKEN_RETRY_GAP   5000UL     // 门禁 token 获取失败后的重试间隔
#define WIFI_LOST_MS     10000       // WiFi 断开超过此时长即整体重连
#define WIFI_PROBE_MS    30000       // WiFi 掉线后做一轮扫描的间隔

Freenove_ESP32_WS2812 strip = Freenove_ESP32_WS2812(LEDS_COUNT, LEDS_PIN, CHANNEL, TYPE_GRB);
CampusInfo gInfo;

// 校园网会话是否在线: 缓存值, 供 /status 用(不在 HTTP handler 里发探测请求)
static bool sCampusOnline = false;

void ledColor(int r, int g, int b) { strip.setAllLedsColor(r, g, b); }

void ledBlink(int r, int g, int b, int times, int halfMs = 150)
{
    for (int i = 0; i < times; i++) {
        ledColor(r, g, b);
        delay(halfMs);
        ledColor(0, 0, 0);
        delay(halfMs);
    }
}

// ==================== app.h 里给通道用的入口 ====================

const CampusInfo& appInfo() { return gInfo; }
bool appCampusOnline() { return sCampusOnline; }
void appLed(int r, int g, int b) { ledColor(r, g, b); }
void appLedBlink(int r, int g, int b, int times, int halfMs) { ledBlink(r, g, b, times, halfMs); }

static void printAccount(const CampusInfo& info)
{
    Serial.println("=========== 账户信息 ===========");
    Serial.printf("用户名   : %s\n", info.userName.c_str());
    Serial.printf("余额     : %s\n", info.balance.c_str());
    Serial.printf("剩余流量 : %s\n", info.traffic.c_str());
    Serial.printf("在线设备 : %s\n", info.devices.c_str());
}

// ==================== WiFi ====================

bool connectWifi(uint32_t timeoutMs)
{
    if (WiFi.status() == WL_CONNECTED) return true;
    if (!settingsHasWifi()) {
        Serial.println("[WiFi] 还没配过 SSID, 需要先开配网热点");
        return false;
    }

    // 热点开着时必须 AP+STA 并发, 否则 mode(WIFI_STA) 会把配网热点关掉
    WiFi.mode(provApActive() ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.wifipswd);
    Serial.print("连接 WiFi");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(500);
        Serial.print(".");
        if (provApActive()) provHandleClient();   // 等 WiFi 的几十秒里也要响应手机请求
        cloudPump();                              // (云端这会儿本来就连不上, 只是保持节拍)
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] 连接超时");
        return false;
    }
    Serial.println("[WiFi] 已连接: " + WiFi.localIP().toString());
    return true;
}

// ==================== 校园网 + 门禁 会话建立 ====================
// 上电、掉线重连、长按按键、/apply 都走这里: 认证 → 账户信息 → 校时 → 门禁 token
void campusUpSteps(ApplySteps& st)
{
    ledColor(0, 0, 255);                      // 蓝: 登录中

    st.wifi = connectWifi();
    if (!st.wifi) {
        st.why = "wifi-not-connected";
        Serial.println("[启动] WiFi 未就绪, 稍后重试");
        ledColor(255, 0, 0);
        sCampusOnline = false;
        return;
    }

    Serial.println("\n[开始登录校园网]");
    st.campus = campusLogin(gInfo);
    if (!st.campus) {
        st.why = "campus-login-failed";
        Serial.println("=========== 登录失败 ===========");
        ledColor(255, 0, 0);                  // 红: 失败
        sCampusOnline = false;
        return;
    }
    sCampusOnline = true;
    printAccount(gInfo);

    st.time = doorSyncTime(5);
    if (!st.time) {
        st.why = "time-sync-failed";
        Serial.println("[启动] 校时失败(开门时会自动重试)");
    }

    st.token = doorAcquireToken();
    if (!st.token) {
        st.why = "door-token-failed";
        Serial.println("[启动] 门禁 token 获取失败(开门时会自动重试)");
        ledColor(255, 255, 0);                // 黄: 待重试
        return;                               // 校园网已通, 不算致命
    }
    if (st.time) st.why = "";               // why 只留"第一个致命失败", 校时失败但 token 成功时不报
    ledBlink(0, 255, 0, 3);                   // 绿闪: 全部就绪
}

bool campusUp()
{
    ApplySteps st;
    campusUpSteps(st);
    return st.campus;
}

// ==================== 开门 ====================

unsigned long lastDoorAt = 0;

DoorResult requestOpenDoor(const char* source)
{
    if (millis() - lastDoorAt < DOOR_MIN_INTERVAL) {   // 抖动/重复触发保护
        Serial.println("[开门] 触发过密, 忽略");
        return DOOR_IGNORED;
    }
    lastDoorAt = millis();

    Serial.printf("\n[%s] 开门\n", source);
    ledColor(0, 0, 255);
    DoorResult r = doorOpen();

    // 拿不到 token 多半是校园网会话已失效: 整体重连后再试一次
    if (r == DOOR_ERR_NO_TOKEN) {
        Serial.println("[开门] 会话疑似失效, 重新登录校园网...");
        ledColor(255, 255, 0);
        if (campusUp() || doorAcquireToken()) r = doorOpen();
    }

    switch (r) {
        case DOOR_OK:
            Serial.println("开门成功");
            ledBlink(0, 255, 0, 1, 200);
            break;
        case DOOR_ERR_NO_TOKEN:
            Serial.println("开门失败: 无法获得门禁 token(校园网/CAS 会话无效)");
            ledColor(255, 0, 0);
            break;
        case DOOR_ERR_NO_TIME:
            Serial.println("开门失败: 校时未成功, 时间戳签名不可信(外网不通?)");
            ledColor(255, 0, 0);
            break;
        case DOOR_IGNORED:
            break;                        // 已经在 guard 里打过日志, 灯不动
        case DOOR_ERR_REJECTED:
        default:
            Serial.println("开门失败: 服务端拒绝(核对 code 设备编号 / 学号 / 权限时段)");
            ledColor(255, 0, 0);
            break;
    }
    return r;
}

// ==================== 长按: 重新登录 / 刷新 ====================

void refreshSession(const char* source)
{
    Serial.printf("\n[%s] 刷新会话\n", source);
    if (campusCheckOnline()) {
        // 会话仍有效: 只重读账户信息 + 换一个新的门禁 token
        sCampusOnline = true;
        if (campusRefreshAccount(gInfo)) printAccount(gInfo);
        doorSyncTime(3);
        ledColor(0, 0, 255);
        if (doorAcquireToken()) {
            Serial.println("[刷新] 完成");
            ledBlink(0, 255, 0, 3);
        } else {
            ledColor(255, 0, 0);
        }
    } else {
        sCampusOnline = false;
        campusUp();
    }
}

// ==================== setup ====================

void setup()
{
    Serial.begin(115200);
    delay(1000);

    pinMode(TRIGGER_BUTTON_PIN, INPUT_PULLUP);
    pinMode(MIJIA_PIN, INPUT_PULLDOWN);
    strip.begin();
    strip.setBrightness(20);

    Serial.println("\nESP32-S3 校园网自动登录 + 门禁开门");
    Serial.printf("[诊断] PSRAM: %s(%u字节), 空闲堆: %u/%u 字节\n",
                  psramFound() ? "有" : "无",
                  (unsigned)ESP.getPsramSize(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getHeapSize());

    settingsLoad();          // 先读 NVS, campusInit 要用 cfg.cookie / cfg.portalUrl
    settingsDump();
    campusInit();
    campusSetPump(cloudPump);   // 登录链阻塞期间让巴法云保持心跳/收命令
    provBegin();
    cloudBegin();

    if (!campusUp() && !settingsHasWifi()) {
        // 从没配过 WiFi: 直接开热点等手机来配
        provStartAp("首次启动无 WiFi 配置");
    }

    Serial.printf("[诊断] 启动后空闲堆: %u 字节, loop任务栈余量: %u 字节\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)uxTaskGetStackHighWaterMark(loopTaskHandle));
    Serial.println("\n就绪: 短按开门 / 长按1.5s刷新 / 长按8s开配网热点 / 门磁触发开门");
}

// ==================== loop ====================

bool btnDown = false;         // GPIO0 当前是否按下
bool btnProvFired = false;    // 本次按压是否已触发超长按(配网)
unsigned long btnDownAt = 0;
bool mjState = false;         // GPIO4 上一状态
unsigned long lastMjFire = 0;
unsigned long lastTokenTry = 0;   // 上次尝试取门禁 token 的时刻(失败时限流)
unsigned long lastCheck = 0;
unsigned long wifiLostAt = 0;

void loop()
{
    provHandleClient();       // 热点开着时响应手机(单线程, 每轮处理一个请求)
    cloudTick();              // 巴法云: 维持连接/心跳, 执行到达的开门与配置

    // ---- 按键 ----
    //  超长按 8s: 现场切换配网热点(要立刻给灯, 所以按住时就触发)
    //  松开时按时长分派: ≥1.5s 刷新会话, ≥30ms 开门(更短算抖动)
    bool down = digitalRead(TRIGGER_BUTTON_PIN) == LOW;
    if (down && !btnDown) {               // 按下沿
        btnDownAt = millis();
        btnProvFired = false;
    }
    if (down && !btnProvFired && millis() - btnDownAt >= PROV_PRESS_MS) {
        btnProvFired = true;              // 按住不放只触发一次
        if (provApActive()) {
            provStopAp();
            ledColor(0, 0, 0);
            Serial.println("[配网] 热点已关闭");
        } else {
            provStartAp("长按 8 秒");
            ledBlink(120, 0, 120, 4, 120);   // 紫闪: 配网中
        }
    }
    if (!down && btnDown && !btnProvFired) {
        unsigned long held = millis() - btnDownAt;
        if (held >= LONG_PRESS_MS) refreshSession("长按按键");
        else if (held >= PRESS_BOUNCE_MS) requestOpenDoor("按键");
    }
    btnDown = down;

    // ---- 外部触发(米家门磁): 上升沿开门 ----
    bool mj = digitalRead(MIJIA_PIN) == HIGH;
    if (mj && !mjState && millis() - lastMjFire >= MIJIA_GAP) {
        lastMjFire = millis();
        requestOpenDoor("门磁");
    }
    mjState = mj;

    // ---- 门禁 token 到期刷新(1 小时) ----
    if (doorTokenExpired() && millis() - lastTokenTry >= TOKEN_RETRY_GAP) {
        lastTokenTry = millis();
        Serial.println("[定时] 刷新门禁 token");
        if (!doorAcquireToken()) ledColor(255, 255, 0);
    }

    // ---- 每 30 分钟检测在线状态 ----
    if (millis() - lastCheck >= CHECK_INTERVAL) {
        lastCheck = millis();
        if (!campusCheckOnline()) {
            Serial.println("[定时] 已掉线, 自动重新登录");
            sCampusOnline = false;
            campusUp();
        } else {
            sCampusOnline = true;
            Serial.println("[定时] 在线状态正常");
            if (campusRefreshAccount(gInfo)) printAccount(gInfo);
        }
    }

    // ---- WiFi 断线恢复 ----
    // STA 掉了 arduino 不会自己重连(路由器重启 / AP 漫游都会造成永久掉线)。
    // WiFi.begin() 内部会先扫描再连, 所以这里只要重连 + 整链重登;
    // 千万别硬套上次的 bssid/channel —— 路由器重启后 bssid 就变了, 反而永远连不上。
    if (WiFi.status() != WL_CONNECTED) {
        if (wifiLostAt == 0) wifiLostAt = millis();
        else if (millis() - wifiLostAt >= WIFI_LOST_MS) {
            wifiLostAt = 0;
            Serial.println("[WiFi] 已断开, 重新连接并登录");
            WiFi.disconnect(false);
            campusUp();
        }
    } else {
        wifiLostAt = 0;
    }

    delay(10);
}
