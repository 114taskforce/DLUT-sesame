# DLUT-sesame（芝麻开门）

大工宿舍门禁的一体化实现：**ESP32-S3 固件**（[`ESP/`](ESP/)）+ **Android App**（[`Android/`](Android/)）。
设备挂在校园网上自动登录、保持门禁 token；手机 App 负责账号登录与一键把配置同步给设备；
可选接巴法云 MQTT，让**小爱同学一句话开门**。

> 本仓库由 [114taskforce/DLUT-door-opener](https://github.com/114taskforce/DLUT-door-opener) 重构而来：
> 原仓库只有"单机版"固件与旧版 App，这里改为 **`ESP/`（固件）+ `Android/`（App）一体化布局**并继续维护。
> 仅作学习与个人自用，请勿用于影响公共设施正常运行等用途。

## 功能

**固件（ESP32-S3）**
- 校园网门户自动登录与保活：断线 30 分钟内自动重登（认证/CAS 换票每一步带日志标号，好排错）
- 门禁换票（CAS → shfb-token → menjin 开门签名请求），门锁状态与余额回读
- 三种触发开门：按键（GPIO0 短按）、门磁/外部按钮（GPIO4 上升沿）、云端消息
- 状态灯（WS2812×3）：登录中/成功/待重试/失败/配网热点中
- 配网与调试：自开热点 `DLUT-Door-xxxx` + 本地 HTTP（`/status` `/save` `/apply` `/open`），浏览器也能手工配

**App（Android）**
- 一键开门大按钮 + 实时状态；自动开门（30 秒内去重）；保留后台/退出进程开关
- CAS 账号密码登录；「网页登录」抓 Cookie 兜底二次认证（短信/验证码）
- 一键**同步到门禁设备**：推 账号/密码/门锁编号/信任 Cookie/巴法参数 → 设备立即重认证
- 自动获取门锁编号、Cookie 持久化、token 失效自动重登重试

**云端（可选）**
- 设备常挂巴法云 MQTT 长连接：米家绑定后喊 **"小爱同学，打开宿舍门"**；远程配置下发与回执

## 仓库结构

```
DLUT-sesame/
├─ Android/                  Android App 工程（Kotlin + Jetpack Compose + OkHttp）
│   ├─ app/                  com.dlut.dooropener 主模块
│   ├─ README.md             App 说明与构建（JDK 17 + Android SDK 36 / Android Studio）
│   └─ CONFIG.md             ★ 完整配置方法（固件 + App + 巴法云 + 小爱）
├─ ESP/                      ESP32-S3 固件工程（PlatformIO / Arduino，env:campusnet）
│   ├─ src/                  main.cpp、campusnet、door、prov、cloud、settings、config.h
│   ├─ platformio.ini        16MB flash；禁用 PSRAM（mbedTLS DMA 堆损坏教训，勿开）
│   └─ docs/app-api.md       设备端 HTTP / 云协议契约（写给 App 的字段表与错误码）
├─ LICENSE                   MIT
└─ README.md                 本文件
```

## 工作方式

```
手机 App ──局域网 HTTP──▶ ESP32 ──校园网门户认证──▶ auth.dlut.edu.cn
   │                       │  └─ CAS 换票 → menjin.dlut.edu.cn 门禁
   └── MQTT(mqtt.bemfa.com:9501) ──▶ 巴法云 ◀── 米家/小爱同学
```

设备平时不暴露配置端口：需要重配时**长按 GPIO0 8 秒**开热点，手机连上
`DLUT-Door-xxxx`（口令 = 设备 PIN），App 一键同步。校园网掉线时设备会自动重登。

## 快速开始

按顺序做完会得到：小爱一句话开门 + 校园网自动登录 + 手机随时改配置。

**① 烧固件**（需 [PlatformIO](https://platformio.org)、ESP32-S3 16MB 开发板）：

```bash
cd ESP
# 出厂默认集中在 ESP/src/config.h：填你的学号/密码/设备编号，改掉默认 PIN
pio run -t upload        # 编译并烧录
pio device monitor       # 115200，看 [0]–[11] 登录步日志
```

**② 装 App**：到本仓库 [Releases](../../releases) 下载 `app-release.apk`（允许"未知来源"直接装），
或按 [`Android/README.md`](Android/README.md) 自编译。

**③ 配对与接小爱**：跟着 [`Android/CONFIG.md`](Android/CONFIG.md) 的 7 节顺序做——
固件填出厂值 → App 登录自己的账号 → 长按 8 秒开热点一键同步 → 巴法云建三个主题 → 绑定米家。

## 文档导航

| 文档 | 内容 |
|---|---|
| [Android/CONFIG.md](Android/CONFIG.md) | ★ 完整配置方法（含固件、巴法云、小爱接入、验收清单、故障排查、安全提醒） |
| [Android/README.md](Android/README.md) | App 功能与构建说明 |
| [ESP/docs/app-api.md](ESP/docs/app-api.md) | 设备端 HTTP / 云 MQTT 协议契约（字段表、错误码、云分帧格式） |

## 安全提醒

- 设备 PIN（`DEVICE_PIN`）既是热点 WPA2 密码也是所有配置接口的鉴权码——**出厂默认值烧录后请立即改掉**。
- 巴法链路是明文 MQTT：私钥与 `CFG_TOPIC`/`ACK_TOPIC` 用你自己的长随机值，不要沿用文档示例。
- 账号密码与 Cookie 明文存于设备 NVS / App 私有存储；设备转手前请 `pio run -t erase`。
- 本项目的对接协议（门户、CAS、门禁接口）来自抓包/逆向，可能与官方服务实现不一致，请仅用于你有权限的门。

## 致谢

- 部分代码由 AI 辅助编写与调试（与旧仓库一致）。
- 旧版"单机固件"演示视频：<https://www.bilibili.com/video/BV1QXXvBEEcq/>（针对旧版流程，新架构以本文档为准）。

## License

[MIT](LICENSE) © 2026 114taskforce
