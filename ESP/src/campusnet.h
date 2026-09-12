#ifndef CAMPUSNET_H
#define CAMPUSNET_H

#include <Arduino.h>

struct CampusInfo {
    bool online = false;
    String userName;   // 学号
    String balance;    // 套餐&余额
    String traffic;    // 剩余流量
    String devices;    // 在线设备
};

// 初始化(NVS 中读取已保存的认证入口地址, 供"已在线"状态下复用)
void campusInit();

// 完整校园网登录流程: 认证跳转 → eportal 会话 → CAS SSO → 上线 → 获取账户信息
// 成功返回 true 并填充 info(余额/剩余流量/在线设备)
bool campusLogin(CampusInfo &info);

// 用当前会话检查是否在线(供定时检测)
bool campusCheckOnline();

// 重新拉取账户信息(余额/剩余流量/在线设备)→ 填充 info。
// 需在 campusLogin 成功后(会话有效、持有 /eportal/ 作用域 JSESSIONID)调用,
// 返回 true 表示成功取到至少一项。
bool campusRefreshAccount(CampusInfo &info);

// ==================== 共享会话原语(供门禁 door.cpp 复用) ====================
// 校园网登录后 sso.dlut.edu.cn 已持有 CASTGC 登录态, 其它校内业务系统(门禁)
// 可直接凭它换 CAS 票据, 无需再提交一次账号密码。下列接口共享同一份
// 分作用域 cookie jar, 因此必须成对使用: campusHttp() 之后紧跟 campusSaveCookies()。
//
// 账号密码 / WiFi / 门禁设备编号统一放在 config.h, 本头文件不再对外暴露。

// 完整 URL 编码(RFC 3986 保留字符全转义), 用于查询参数值
String campusUrlEncode(const String& s);

// 统一 HTTP/HTTPS 请求(setInsecure, 手工重定向, 自动带 host+path 匹配的 cookie)
// 返回完整原始响应(含状态行/首部/正文); extraHeaders 每行需自带 \r\n
String campusHttp(const String& url, const String& method = "GET",
                  const String& body = "", const String& contentType = "",
                  const String& extraHeaders = "");

// 解析响应的 Set-Cookie(含 Path/Domain)入共享 jar —— 必须紧跟 campusHttp
void campusSaveCookies(const String& res);

// 响应头 Location / JS 跳转 / meta refresh 解析出的跳转地址(相对 baseUrl 补全), 无则返回 ""
String campusLocation(const String& res, const String& baseUrl);

// 响应正文(去掉首部)
String campusBody(const String& res);

// 读 host 下指定 cookie 的值(按 path 最长优先), 不存在返回 ""
String campusCookie(const String& host, const String& name);

// 二次认证兜底: 把浏览器复制的 Cookie 头原文(config.h 的 COOKIE_INPUT)注入共享 jar。
// 只注入 COOKIE_TRUSTED 白名单里的名字(默认 CASTGC / JSESSIONIDCAS), 作用域
// sso.dlut.edu.cn + Path=/ —— 全量灌进去会让 CAS 不再下发带 lt/execution 的登录页。
// CASTGC 有效时 /cas/login?service= 直接签票据, 需要验证码的表单分支就不会走到。
// campusInit() 登记并注入一次, campusLogin() 清空 jar 后再灌一次。
void campusSeedCookies(const String& cookieHeader);

// 从 COOKIE_INPUT 原文里按名字取单个 cookie 值(不查 jar, 也不受白名单限制),
// 没有返回 ""。门禁换票失败时用它取 shfb-token 兜底。
String campusSeedCookie(const String& name);

// 实际注入会话的种子条数(白名单过滤后), 供 /status 回显
int campusSeedApplied();

// CAS 服务票据: 为 service 换取 ticket=ST-... 完整回跳地址。
// 优先复用已有 CASTGC(两次请求即得); 无登录态时自动提交账号密码后再换。
// 持有的 CAS 会话已失效(拿不到票且 CAS 不下发表单)时, 会先丢掉 jar 里
// sso 域的 CASTGC/JSESSIONIDCAS 再重新取页, 让账号密码兜底能正常走。
// 成功返回 true, ticketUrl 为含 ticket 的 service 地址。
bool campusCasTicket(const String& service, String& ticketUrl);

// ==================== 长流程让出钩子 ====================
// 整条登录链是阻塞的(最长几十秒), 期间云通道需要发包收包(否则被服务器判掉线)。
// 注册一个回调, campusnet 在每次 HTTP 收包的间隙、door 在每次重试等待里调它。
// 回调只许"收包+发包", 不许在里面再触发一次登录/开门(cloud.cpp 已按此实现)。
typedef void (*CampusPump)();
void campusSetPump(CampusPump p);
void campusYield();                 // 供 door.cpp 等长等待处调用

#endif
