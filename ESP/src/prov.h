#ifndef PROV_H
#define PROV_H

#include <Arduino.h>

// ====================================================================
// 本地配网 / 调试通道: 设备自开 SoftAP + 内置 WebServer。
// 冷启动(还没配过 WiFi)时自动开, 也可长按 GPIO0 8 秒手动开。
// 接口契约见 docs/app-api.md 的 1。
//
// 为什么不用 BLE 配网: BLE 控制器与主机要吃掉几十 KB 内部 RAM, 而本项目禁用
// PSRAM、mbedTLS 登录链峰值已约 250KB/320KB, 再叠一层协议栈就是抢 TLS 的空间。
// ====================================================================

void provBegin();                        // 注册路由(不开热点)
void provStartAp(const char* why);       // 开热点(AP+STA 并发, STA 保持在线)
void provStopAp();
void provHandleClient();                 // loop() 每轮调用
bool provApActive();

#endif
