#ifndef DOOR_H
#define DOOR_H

#include <Arduino.h>

// 开门结果
enum DoorResult {
    DOOR_OK = 0,       // 服务端受理
    DOOR_ERR_NO_TOKEN, // 拿不到 token(校园网/CAS 会话失效), 需重新登录校园网
    DOOR_ERR_REJECTED, // 有 token 但服务端拒绝(设备号/人员/时间戳/权限)
    DOOR_ERR_NO_TIME,  // 校时没成功, 签名里的时间戳不可信(外网不通)
    DOOR_IGNORED       // 被触发保护吞掉(1.5 秒内重复按下), 没有发出请求
};

// 毫秒时间戳校时(苏宁接口)。校园网未认证时访问不到外网, 必须在登录之后调用。
bool doorSyncTime(int maxRetries = 5);

// 取门禁会话 token(shfb-token): 复用校园网 CASTGC 换 CAS 票据 → 请求门禁首页落地 cookie
bool doorAcquireToken();

// token 是否为空或已超过 1 小时(供主程序定时刷新)
bool doorTokenExpired();

// 开门: 失败时自动重取 token 重试一次
DoorResult doorOpen();

// ==================== 状态查询(本地 HTTP /status 与云回执用) ====================

bool doorTimeSynced();               // 是否已校时
unsigned long doorTokenAgeMs();      // 距上次拿到 token 过了多久(无 token 返回 0)
const char* doorLastResp();          // 最近一次开门的服务端响应(截到 199 字节)
bool doorLastResult(DoorResult* out);// 是否已开过一次; out 可为 NULL 只问"有没有"
unsigned long doorLastAgeMs();       // 距上次开门请求多久(没开过返回 0)

#endif
