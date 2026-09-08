#ifndef APP_H
#define APP_H

#include <Arduino.h>
#include "campusnet.h"
#include "door.h"

// ====================================================================
// main.cpp 提供的编排入口。本地配网(prov)和云通道(cloud)都调这几个函数,
// 不各自实现一份"登录/开门", 免得三条通道的行为分叉。
// ====================================================================

// /apply 的分步结果(docs/app-api.md 1.3)。why 指向失败原因短语, 成功时为空。
struct ApplySteps {
    bool wifi = false;
    bool campus = false;
    bool time = false;
    bool token = false;
    const char* why = "";
};

bool connectWifi(uint32_t timeoutMs = 30000);   // 只连 WiFi, 不做门户认证
bool campusUp();                                // WiFi → 认证 → 校时 → 门禁 token
void campusUpSteps(ApplySteps& out);            // 同上, 但把每步结果回给调用方
DoorResult requestOpenDoor(const char* source); // 开门(会话失效会自愈重试)
void refreshSession(const char* source);        // 刷新会话与账户信息
const CampusInfo& appInfo();                    // 最近一次读到的余额/流量/设备
bool appCampusOnline();                         // 校园网会话是否在线(缓存值, 不发请求)
void appLed(int r, int g, int b);               // 状态灯(通道里用)
void appLedBlink(int r, int g, int b, int times, int halfMs);

#endif
