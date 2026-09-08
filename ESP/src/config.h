// ====================================================================
//  出厂默认 —— 账号 / WiFi / 门禁设备 / 云主题
//
//  这里改的是"烧进去的初始值"。同一批字段手机可以远程覆盖(见 docs/app-api.md),
//  覆盖值存在 NVS 里、断电不丢, 优先级高于本文件; 想让本文件的值重新生效,
//  用 App 的"恢复出厂"或擦 NVS。改本文件仍需重新 pio run -t upload。
//  (硬件引脚与定时间隔在 main.cpp 顶部的 "硬件" / "周期与时长" 两段)
// ====================================================================

#ifndef CONFIG_H
#define CONFIG_H

// -------------------- 校园网 / 统一身份认证 --------------------
// 学号, 同时用作门禁的 personId。登录 sso.dlut.edu.cn 与 auth.dlut.edu.cn 门户
// 都只有这一处提交账号密码: 门禁靠已登录的 CASTGC 换票据, 不再单独提交。
#define CAMPUS_USER  ""
#define CAMPUS_PASS  ""

// -------------------- 二次认证兜底: 基础 Cookie (可选, 默认不用) --------------------
// 统一身份认证对脚本登录可能弹二次认证(短信/验证码), 表单提交过不去时, 把浏览器
// 已登录的 cookie 粘到这里绕过去: 浏览器登录 sso.dlut.edu.cn → F12 Network →
// 任意请求 → 复制整条 Cookie 请求头 → 粘进下面的引号里。
// 它作为**基础 cookie** 参与会话: 服务端随后返回的 Set-Cookie 继续往它上面加,
// 同名的就地覆盖(所以 CASTGC 过期后会被新签发的那条替换, 不用手动更新)。
// CAS 认这个 CASTGC 便直接签发票据, 需要验证码的表单分支根本不会走到。
//
// 注意: 只有下面 COOKIE_TRUSTED 列出的名字会被真正注入会话, 且只作用于
// sso.dlut.edu.cn —— 粘贴串里的 recheck_mobile_error_info / djsendtime_recheck /
// devInfo 之类会改变 CAS 对登录页的响应(不再下发带 lt/execution 的表单), 全量灌进去
// 反而会把原本能用的登录链搞挂。日志里 "白名单外跳过 N 条" 会列出被忽略的名字;
// 若换票失败, 把需要的名字加进 COOKIE_TRUSTED 再试。
// 未注入的名字仍可按名取用: 门禁换票失败时 door.cpp 会用串里的 shfb-token 兜底
// (浏览器里该 cookie 是 HttpOnly 的话, 去 Application → Cookies 里抄)。
// COOKIE_INPUT 留空 "" = 不使用。
#define COOKIE_INPUT  ""

// 允许注入会话的 cookie 名(空格分隔, 大小写敏感)
#define COOKIE_TRUSTED  "CASTGC JSESSIONIDCAS"

// -------------------- WiFi --------------------
// DLUT-LingShui 是开放网络, 密码留空即可
#define WIFI_SSID    "DLUT-LingShui"
#define WIFI_PSWD    ""

// -------------------- 门禁 (menjin.dlut.edu.cn) --------------------
// 宿舍门设备编号: 抓包 sendRoomBatch 请求体里的 deviceCode 字段
#define DOOR_DEVICE_CODE  "DL-LY-000000"

// 项目码(全校固定, 大连理工门禁), 一般不用改
#define DOOR_PROJECT_CD   "DA_LIAN_LI_GONG_MENJIN"

// 签名盐, 前端 JS 里硬编码的那串, 服务端换版本才可能变
#define DOOR_SIGN_SALT    "2323dsfadfewrasa3434"

// -------------------- 设备口令与云通道(手机可覆盖) --------------------
// DEVICE_PIN: 既是本地配网热点的 WPA2 口令, 也是所有 HTTP/云报文的鉴权码。
// 长度必须 8~16 位(短于 8 位开不了 WPA2 热点), 只能数字字母。
// 首次配网时 App 用 /save 换成自己的; 忘了只能擦 NVS 重来。
#define DEVICE_PIN   "47124712"

// BEMFA_KEY: 巴法云(cloud.bemfa.com)控制台首页的"用户私钥", 留空 = 不开云通道。
#define BEMFA_KEY    ""

// 三个主题名(先在巴法控制台建好 —— 协议类型选 MQTT, 再填这里或用 App 写入):
//   DOOR_TOPIC  控制主题, 要出现在米家里 → 名字必须是纯字母数字且**后三位是设备
//               类型码**(001=插座/开关类), 否则米家看不见它。
//   CFG_TOPIC   配置下行(推学号/密码/设备编号/cookie), 私有名字, 别和上面重复。
//   ACK_TOPIC   设备回执, App 订阅它确认写入成功。
// 主题名只能字母数字、≤64 字符。注意: 设备端只订阅"裸主题", 带 /set 或 /up 后缀的
// 订阅会被服务器拒(SUBACK 0x80) —— 那两个后缀是发布用的。
#define DOOR_TOPIC   ""
#define CFG_TOPIC    "dlutcfg4712"
#define ACK_TOPIC    "dlutack4712"

#endif
