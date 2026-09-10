# 宿舍门(大工宿舍门禁 App)

*本目录是仓库的 `Android/` 子工程，固件在 `ESP/`，总览见根目录 [README](../README.md)。*

**完整配置方法(含 ESP32 固件那一侧、巴法云与小爱接入)见 [CONFIG.md](CONFIG.md)。**

从 ESP32 固件(`main.cpp` / `des.cpp`)移植到 Android 的门禁客户端:Kotlin + Jetpack Compose + OkHttp,协议与固件保持一致(CAS 登录、AUTH-SIGN 签名、DES 加密)。

## 功能

- 一键开门(大圆形按钮 + 状态提示)
- CAS 账号密码登录;支持「网页登录」解决二次认证(信任 Cookie 预置,等价固件 `COOKIE_INPUT`)
- 自动获取门锁编号(也可手动输入)
- 自动开门:打开 APP 或从后台回到前台时触发,10 秒内去重
- 保留后台 / 退出进程开关
- **WebVPN 开关**:不在校园网时打开,先登录 webvpn.dlut.edu.cn 再访问门禁(开门、取编号都走网关);
  关闭时直连门禁,不尝试打开 VPN
- Cookie 持久化,Token 失效自动重新登录并重试
- **同步到门禁设备**:设置页一键把账号密码、门锁编号、信任 Cookie、巴法云参数写给
  ESP32 固件并让它立即重新认证;别人拿到设备只要连热点 + 填口令 + 点同步

## 给新设备配网(同步到设备)

详见 [CONFIG.md](CONFIG.md) 第 3 节。简版:长按设备按键 8 秒开热点 → 手机连
`DLUT-Door-xxxx` → 设置页填设备口令 → 「同步到设备」(推 账号密码 / 门锁编号 /
信任 Cookie / 巴法云参数,并让设备立即重新认证) → 「试开门」核对设备编号。

> 设备侧接口契约见 [ESP/docs/app-api.md](../ESP/docs/app-api.md)。Android 的 network security config
> 不匹配 IP 字面量,所以 [network_security_config.xml](app/src/main/res/xml/network_security_config.xml)
> 放开了明文(仅影响局域网设备与本来就是 http 的门禁接口,sso 仍是 HTTPS)。

## 构建

需要 JDK 17 + Android SDK(compileSdk 36),或用 Android Studio 打开。

```bash
./gradlew assembleDebug    # 调试版
./gradlew assembleRelease  # 发布版
```

> release 签名密钥在本机生成、不随仓库分发。构建发布版前先生成自己的密钥:
>
> ```bash
> keytool -genkeypair -v -keystore release.keystore -alias doorapp \
>   -keyalg RSA -keysize 2048 -validity 10000
> ```
>
> 并在项目根目录创建 `keystore.properties`(内容见 [app/build.gradle.kts](app/build.gradle.kts) 顶部注释)。

## 下载

见 [Releases](../../releases) 页面,下载 `app-release.apk` 直接安装(需允许「安装未知来源应用」)。

## 说明

- 仅供个人学习与自用,请勿用于影响公共设施正常运行等用途。
- 密码以明文存于应用私有 SharedPreferences,个人工具够用;介意可换 EncryptedSharedPreferences。
