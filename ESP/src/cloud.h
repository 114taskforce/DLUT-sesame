#ifndef CLOUD_H
#define CLOUD_H

#include <Arduino.h>

// ====================================================================
// 巴法云通道(cloud.bemfa.com) —— 小爱同学开门 + 手机远程改配置。
//
// 用标准 MQTT 3.1.1 而不是巴法的私有 TCP(8344): 米家能同步的是控制台里
// "MQTT 设备云" 类型的主题, 走 TCP 建的主题在米家里看不见。
// 实测(2026-09-08): bemfa.com:9501 CONNACK rc=0, clientId=私钥, 无用户名密码;
// 1883 端口是关的。主题必须先在控制台建好, 否则 SUBACK 返回 0x80。
//
// 报文是手写的最小 MQTT(CONNECT/SUBSCRIBE/PUBLISH/PINGREQ), 不引第三方库:
// 本项目零 lib_deps, 且手写能精确控制缓冲区大小(禁用 PSRAM, 内部 RAM 紧张)。
// ====================================================================

void cloudBegin();                 // 读配置, 不主动连接(等校园网通了再连)
void cloudTick();                  // loop() 每轮调用: 维持连接/心跳/收消息
// 长流程(登录链)里只能 "只收不执行": 命令进单槽队列, 回到主循环后再执行
void cloudPump();

bool cloudConnected();
const char* cloudState();          // disabled | connecting | connected | down

#endif
