// ====================================================================
// 大连理工大学校园网自动登录 (eportal v2 + CAS SSO)
// 流程参考浏览器抓包:
//   123.123.123.123(被拦截) → auth.dlut.edu.cn/eportal/index.jsp
//   → /portal/portal-main(sessionId) → CAS SSO(sso.dlut.edu.cn)
//   → /eportal/network/userOnline → /eportal/operator/getAccountInfo
// CAS 表单 "rsa" 字段加密与门禁登录一致: strEnc(账号+密码+lt, "1","2","3")
// ====================================================================

#include "campusnet.h"
#include "config.h"      // 出厂默认 + COOKIE_TRUSTED 白名单
#include "settings.h"    // 运行时配置 cfg.user / cfg.pass / cfg.cookie / cfg.portalUrl
#include "des.h"

#include <WiFiClientSecure.h>

// ==================== 服务器地址 ====================
const char* PROBE_URL = "http://123.123.123.123/";   // 未认证时任意HTTP会被拦截跳转
const char* AUTH_HOST = "http://auth.dlut.edu.cn";
const char* SSO_HOST  = "https://sso.dlut.edu.cn";
const char* AUTH_HOSTNAME = "auth.dlut.edu.cn";   // cookie 作用域 host(不带 scheme)
const char* SSO_HOSTNAME  = "sso.dlut.edu.cn";

static const char* USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/152.0.0.0 Safari/537.36 Edg/152.0.0.0";

// 分作用域 Cookie: 抓包证实 auth.dlut.edu.cn 同时存在 /sam/ 与 /eportal/ 两条同名
// JSESSIONID, 浏览器靠 Set-Cookie 的 Path 分流; 扁平 jar 无法表示, 故按 (host,path,name) 存储
#define MAX_COOKIES 24
struct ScopedCookie {
    String host;   // 小写 host(Domain 属性或缺省下发请求的 host)
    String path;   // Set-Cookie 的 Path 属性, 缺省=请求路径目录
    String name;
    String value;
    bool seed = false;   // true = 来自 config.h COOKIE_INPUT 的基础 cookie, 服务端亲发的同名 cookie 会顶掉它
};
static ScopedCookie gCookies[MAX_COOKIES];
static int gCookieCount = 0;
static String gLastHost;     // 最近一次 httpRequest 的 host(小写, saveCookie 归属用)
static String gLastPath;     // 最近一次 httpRequest 的 path
static String gSessionId;    // eportal sessionId
static String gUserMac;      // 设备 MAC(小写无冒号)
static String gCustomPageId; // portal-main URL 中的 customPageId
static String gUserIp;       // portal-main URL 中的 userIp
static String gNasIp;        // portal-main URL 中的 nasIp

// NVS 统一由 settings.cpp 持有(命名空间 "campus"), 本文件只读写 cfg 里的字段

// ==================== 长流程让出钩子 ====================
static CampusPump sPump = nullptr;
void campusSetPump(CampusPump p) { sPump = p; }
void campusYield() { if (sPump) sPump(); }

// ==================== 基础工具 ====================

// 简单URL编码(仅处理HTTP请求行中不允许的字符)
static String urlEncodePath(const String& s)
{
    String out;
    out.reserve(s.length() + 8);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c == ' ') out += "%20";
        else if (c == '"') out += "%22";
        else if (c == '<') out += "%3C";
        else if (c == '>') out += "%3E";
        else if (c == '|') out += "%7C";
        else out += c;
    }
    return out;
}

// 完整 URL 编码(用于 service 等查询参数值, RFC 3986 保留字符全部转义)
static String urlEncodeQuery(const String& s)
{
    const char* hex = "0123456789ABCDEF";
    String out;
    out.reserve(s.length() * 3);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out += c;
        else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// 打印空闲堆(定位 TLS 阶段堆压力)
static void printHeap(const char* tag)
{
    Serial.printf("[内存] %s: 空闲堆 %u 字节\n", tag, (unsigned)ESP.getFreeHeap());
}

// 截断打印(调试)
static void printTrunc(const char* tag, const String& s)
{
    Serial.printf("[%s] (%u 字节)\n", tag, s.length());
    if (s.length() <= 1000) Serial.println(s);
    else Serial.println(s.substring(0, 1000) + "\n...截断");
}

// HTTP/HTTPS 请求(手动处理重定向; extraHeaders 每行需自带 \r\n)
static String cookieHeader(const String& host, const String& path);   // 定义在 Cookie 管理段
static String httpRequest(const String& url, const String& method,
                          const String& body, const String& contentType,
                          const String& extraHeaders, uint32_t timeoutMs = 20000)
{
    // 解析 URL
    String rest = url;
    String scheme = "http";
    int p = url.indexOf("://");
    if (p != -1) { scheme = url.substring(0, p); rest = url.substring(p + 3); }

    String host = rest;
    String path = "/";
    int slash = rest.indexOf('/');
    if (slash != -1) { host = rest.substring(0, slash); path = rest.substring(slash); }
    path = urlEncodePath(path);
    gLastHost = host;
    gLastHost.toLowerCase();
    gLastPath = path;

    uint16_t port = (scheme == "https") ? 443 : 80;

    WiFiClientSecure secClient;
    WiFiClient plainClient;
    Client* client = nullptr;
    if (scheme == "https") {
        secClient.setInsecure();
        client = &secClient;
    } else {
        client = &plainClient;
    }

    if (!client->connect(host.c_str(), port)) {
        Serial.printf("[HTTP] 连接失败 %s://%s:%u\n", scheme.c_str(), host.c_str(), port);
        return "";
    }

    // 构造请求(不发送 Accept-Encoding, 避免收到gzip)
    String req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "User-Agent: " + String(USER_AGENT) + "\r\n";
    req += "Accept: */*\r\n";
    req += "Connection: close\r\n";
    String ck = cookieHeader(host, path);
    if (ck.length() > 0)
        req += "Cookie: " + ck + "\r\n";
    if (method == "POST") {
        req += "Content-Type: " + contentType + "\r\n";
        req += "Content-Length: " + String(body.length()) + "\r\n";
    }
    req += extraHeaders;
    req += "\r\n";
    req += body;

    client->print(req);

    // 读取响应
    String res;
    res.reserve(2048);
    uint8_t buf[1024];
    unsigned long start = millis();
    while (client->connected() || client->available()) {
        if (client->available()) {
            int n = client->read(buf, sizeof(buf));
            if (n > 0) res.concat((const char*)buf, (unsigned int)n);
        } else {
            delay(1);
            campusYield();      // 收包间隙让云通道发心跳/收命令(它只收发, 不会重入登录)
        }
        if (millis() - start > timeoutMs) {
            Serial.println("[HTTP] 读取超时");
            break;
        }
    }
    client->stop();
    return res;
}

// ==================== Cookie 管理 ====================

// 取请求路径的目录部分(Set-Cookie 缺省 Path 规则)
static String defaultCookiePath(const String& path)
{
    int slash = path.lastIndexOf('/');
    if (slash <= 0) return "/";
    return path.substring(0, slash);
}

// 两个 host 是否同一站点(相等, 或一方是另一方的 Domain 后缀形式)
static bool hostRelated(const String& a, const String& b)
{
    if (a == b) return true;
    String da = a.startsWith(".") ? a : ("." + a);
    String db = b.startsWith(".") ? b : ("." + b);
    return b.endsWith(da) || a.endsWith(db);
}

// 存/更新一条 (host,path,name) 作用域的 cookie。
// isSeed=true 表示这是 config.h COOKIE_INPUT 里的基础 cookie: 它只是起点,
// 服务端随后亲发的同名 cookie 会把它顶掉; 反过来已有服务端 cookie 时种子不再插入。
static void setCookie(const String& host, const String& path, const String& name,
                      const String& value, bool isSeed = false)
{
    if (value.length() == 0) return;
    for (int i = gCookieCount - 1; i >= 0; i--) {
        if (gCookies[i].name != name || !hostRelated(gCookies[i].host, host)) continue;
        if (gCookies[i].seed && !isSeed) {          // 服务端亲发 → 删掉基础 cookie
            for (int j = i; j < gCookieCount - 1; j++) gCookies[j] = gCookies[j + 1];
            gCookieCount--;
        } else if (!gCookies[i].seed && isSeed) {   // 已有服务端值 → 种子让路
            return;
        }
    }
    for (int i = 0; i < gCookieCount; i++) {
        if (gCookies[i].host == host && gCookies[i].path == path && gCookies[i].name == name) {
            gCookies[i].value = value;
            gCookies[i].seed = isSeed;
            return;
        }
    }
    if (gCookieCount < MAX_COOKIES) {
        gCookies[gCookieCount].host = host;
        gCookies[gCookieCount].path = path;
        gCookies[gCookieCount].name = name;
        gCookies[gCookieCount].value = value;
        gCookies[gCookieCount].seed = isSeed;
        gCookieCount++;
    } else if (isSeed) {
        Serial.printf("[Cookie种子] 槽位已满(%d), 后面的种子未注入\n", MAX_COOKIES);
    }
}

// 解析响应中的 Set-Cookie(含 Path/Domain 属性), 归属到最近一次请求的 host/path
static void saveCookie(const String& res)
{
    int p = 0;
    while ((p = res.indexOf("Set-Cookie:", p)) != -1) {
        p += 11;
        while (p < (int)res.length() && res.charAt(p) == ' ') p++;
        int eol = res.indexOf("\r\n", p);
        if (eol == -1) eol = res.length();
        String line = res.substring(p, eol);
        p = eol;
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String name = line.substring(0, eq);
        String value = line.substring(eq + 1);
        // 解析属性(Path / Domain), 其余(Max-Age/HttpOnly/Secure 等)忽略
        String ckPath = defaultCookiePath(gLastPath);
        String ckHost = gLastHost;
        int attrs = value.indexOf(';');
        if (attrs != -1) {
            String rest = value.substring(attrs + 1);
            value = value.substring(0, attrs);
            int a = 0;
            while (a < (int)rest.length()) {
                int semi = rest.indexOf(';', a);
                if (semi == -1) semi = rest.length();
                String attr = rest.substring(a, semi);
                a = semi + 1;
                attr.trim();
                String al = attr;
                al.toLowerCase();
                if (al.startsWith("path=")) ckPath = attr.substring(5);
                else if (al.startsWith("domain=")) {
                    ckHost = attr.substring(7);
                    ckHost.toLowerCase();
                }
            }
        }
        if (value.length() == 0) continue;   // 忽略空值cookie(如 cas_hash=)
        if (ckPath.length() == 0) ckPath = "/";
        setCookie(ckHost, ckPath, name, value);
    }
}

// 构造发给 host+path 的 Cookie 头(只带 host 匹配且 path 前缀匹配的)
static String cookieHeader(const String& host, const String& path)
{
    String out;
    for (int i = 0; i < gCookieCount; i++) {
        // host 匹配: 精确相等或 Domain 后缀匹配(Domain 带前导点)
        bool hostOk = (gCookies[i].host == host);
        if (!hostOk && gCookies[i].host.startsWith(".") && host.endsWith(gCookies[i].host))
            hostOk = true;
        if (!hostOk) continue;
        // path 匹配: cookie.path 是请求 path 的前缀, 且边界为 '/' 或结尾
        const String& cp = gCookies[i].path;
        if (!path.startsWith(cp)) continue;
        if (cp.length() > 1 && cp.charAt(cp.length() - 1) != '/' &&
            path.charAt(cp.length()) != '/')
            continue;
        if (out.length()) out += "; ";
        out += gCookies[i].name + "=" + gCookies[i].value;
    }
    return out;
}

// 从指定 host 的 cookie 中读取指定名称的值(按 path 最长优先, 供内部构造 URL 用)
// host 匹配规则与 cookieHeader 一致(精确相等, 或 Domain 属性带前导点的后缀匹配):
// 否则服务端若用 Domain=.dlut.edu.cn 下发(如门禁 shfb-token), 这里会漏读
static String getCookieValue(const String& host, const String& name)
{
    String best;
    int bestLen = -1;
    for (int i = 0; i < gCookieCount; i++) {
        if (gCookies[i].name != name) continue;
        bool hostOk = (gCookies[i].host == host);
        if (!hostOk && gCookies[i].host.startsWith(".") && host.endsWith(gCookies[i].host))
            hostOk = true;
        if (!hostOk) continue;
        int len = gCookies[i].path.length();
        if (len > bestLen) { bestLen = len; best = gCookies[i].value; }
    }
    return best;
}

// 调试打印全部 cookie
static String dumpCookies()
{
    String out;
    for (int i = 0; i < gCookieCount; i++) {
        if (out.length()) out += "; ";
        out += gCookies[i].name + "=" + gCookies[i].value +
               " [" + gCookies[i].host + gCookies[i].path + "]" +
               (gCookies[i].seed ? "(基础)" : "");
    }
    return out;
}

// ==================== 二次认证兜底: 浏览器 Cookie 种子 ====================
// config.h 的 COOKIE_INPUT 是一整条浏览器 Cookie 头("a=1; b=2")。CAS 对脚本登录弹
// 二次认证(短信/验证码)时表单过不去, 粘一份浏览器已登录的 cookie 进来即可绕过:
// CASTGC 有效时 /cas/login?service= 直接签票据, 表单分支根本不会被走到。
//
// 门户登录(campusLogin)和门禁换票(campusCasTicket)都会注入。带 CASTGC 后 CAS 不再
// 下发带 lt/execution 的表单, 而是直接给一个跳转/无表单页 —— campusLogin [5b] 与
// campusCasTicket ① 就是为这条路准备的; 登录链不再依赖表单页。
static String gSeedCookie;
static int gSeedApplied = 0;      // 实际注入 jar 的种子条数(供 /status 回显)

// 把种子注入共享 jar: 只投 COOKIE_TRUSTED 白名单里的名字, 且只作用于 sso 站。
// 为什么收窄: 粘贴串里那些 recheck_mobile_error_info / djsendtime_recheck / devInfo
// 之类会改变 CAS 对门户登录页的响应(不再下发带 lt/execution 的表单), 把原本能用的
// 登录链搞挂; 而换票据真正需要的只有 CASTGC(外加 JSESSIONIDCAS 拼 session_state)。
// 其余名字不进 jar, 但仍可用 campusSeedCookie() 按名取(门禁 shfb-token 兜底)。
static void applySeedCookies()
{
    int applied = 0, skipped = 0;
    String skippedNames;
    int p = 0;
    while (p < (int)gSeedCookie.length()) {
        int e = gSeedCookie.indexOf(';', p);
        if (e == -1) e = gSeedCookie.length();
        String item = gSeedCookie.substring(p, e);
        p = e + 1;
        item.trim();
        int eq = item.indexOf('=');
        if (eq <= 0) continue;
        String name = item.substring(0, eq);
        String value = item.substring(eq + 1);
        value.trim();
        if (!name.length() || !value.length()) continue;
        if (String(" " COOKIE_TRUSTED " ").indexOf(" " + name + " ") == -1) {
            skipped++;
            if (skippedNames.length()) skippedNames += " ";
            skippedNames += name;
            continue;
        }
        // host = sso(票据只有它认), path = / 以便覆盖 /cas/login 与 /oauth2.0/*
        setCookie(SSO_HOSTNAME, "/", name, value, true);
        applied++;
    }
    if (applied) Serial.printf("[Cookie种子] 注入 %d 条(%s)\n", applied, SSO_HOSTNAME);
    if (skipped) Serial.printf("[Cookie种子] 白名单外跳过 %d 条: %s\n", skipped, skippedNames.c_str());
    gSeedApplied = applied;
}

int campusSeedApplied() { return gSeedApplied; }

// 存下种子并立刻注入一次(campusInit 调用); campusLogin 清空 jar 后会再灌一遍
void campusSeedCookies(const String& cookieHeader)
{
    gSeedCookie = cookieHeader;
    gSeedCookie.trim();
    if (!gSeedCookie.length()) return;
    int n = 0, p = 0;
    while (p < (int)gSeedCookie.length()) {
        int e = gSeedCookie.indexOf(';', p);
        if (e == -1) e = gSeedCookie.length();
        if (gSeedCookie.substring(p, e).indexOf('=') > 0) n++;
        p = e + 1;
    }
    Serial.printf("[Cookie种子] 登记 %d 条, 用于跳过统一身份认证二次认证\n", n);
    if (!n) Serial.println("[Cookie种子] 没解析出 name=value, 检查粘贴内容");
    applySeedCookies();
}

String campusSeedCookie(const String& name)
{
    int p = 0;
    while (p < (int)gSeedCookie.length()) {
        int e = gSeedCookie.indexOf(';', p);
        if (e == -1) e = gSeedCookie.length();
        String item = gSeedCookie.substring(p, e);
        p = e + 1;
        item.trim();
        int eq = item.indexOf('=');
        if (eq <= 0) continue;
        if (item.substring(0, eq) == name) {
            String value = item.substring(eq + 1);
            value.trim();
            return value;
        }
    }
    return "";
}

// ==================== 响应解析 ====================

// 状态行(诊断用: "HTTP/1.1 302 FOUND" / "HTTP/1.1 200 OK")
static String statusLine(const String& res)
{
    int e = res.indexOf("\r\n");
    return (e > 0) ? res.substring(0, e) : "(空响应)";
}

static String getHeader(const String& res, const String& name)
{
    int headEnd = res.indexOf("\r\n\r\n");
    if (headEnd == -1) return "";
    String head = res.substring(0, headEnd);
    String lower = head;
    lower.toLowerCase();
    String ln = name;
    ln.toLowerCase();
    int p = lower.indexOf(ln + ":");
    if (p == -1) return "";
    p += ln.length() + 1;
    while (p < (int)head.length() && (head.charAt(p) == ' ' || head.charAt(p) == '\t')) p++;
    int e = head.indexOf("\r\n", p);
    if (e == -1) e = head.length();
    return head.substring(p, e);
}

static String getBody(const String& res)
{
    int p = res.indexOf("\r\n\r\n");
    if (p == -1) return "";
    return res.substring(p + 4);
}

static String getQueryParam(const String& url, const String& name)
{
    int p = url.indexOf(name + "=");
    if (p == -1) return "";
    p += name.length() + 1;
    int e = url.indexOf('&', p);
    if (e == -1) e = url.length();
    return url.substring(p, e);
}

// 提取跳转目标: 优先 HTTP Location 头, 其次页面 JS 跳转 / meta refresh
// (网关拦截页返回的是 JS 跳转, 不是 302; baseUrl 用于解析相对路径)
static String getRedirectUrl(const String& res, const String& baseUrl)
{
    String loc = getHeader(res, "Location");
    if (loc.length()) {
        // Location 可为相对路径(RFC 7231), 相对 baseUrl 解析
        if (loc.startsWith("/")) {
            String base = baseUrl;
            int s = base.indexOf("://");
            if (s != -1) {
                int e = base.indexOf('/', s + 3);
                if (e != -1) base = base.substring(0, e);
            }
            return base + loc;
        }
        return loc;
    }

    String body = getBody(res);

    // JS: location.href='...' / location.href="..." / location="..."
    int p = body.indexOf("location.href");
    if (p == -1) p = body.indexOf("location=");
    if (p != -1) {
        p = body.indexOf('=', p);
        if (p != -1) {
            p++;
            while (p < (int)body.length() && body.charAt(p) == ' ') p++;
            char q = (p < (int)body.length()) ? body.charAt(p) : 0;
            String url;
            if (q == '\'' || q == '"') {
                p++;
                int e = body.indexOf(q, p);
                if (e == -1) e = body.length();
                url = body.substring(p, e);
            } else {
                int e = body.indexOf(' ', p);
                if (e == -1) e = body.length();
                url = body.substring(p, e);
            }
            if (!url.startsWith("http")) {
                // JS 表达式拼接, 如 window.location.origin + '/path'
                int plus = body.indexOf('+', p);
                if (plus != -1) {
                    int qp = body.indexOf('\'', plus);
                    int qp2 = body.indexOf('"', plus);
                    if (qp == -1 || (qp2 != -1 && qp2 < qp)) qp = qp2;
                    if (qp != -1) {
                        char qc = body.charAt(qp);
                        int e2 = body.indexOf(qc, qp + 1);
                        if (e2 == -1) e2 = body.length();
                        url = body.substring(qp + 1, e2);
                    }
                }
            }
            if (url.startsWith("http")) return url;
            if (url.startsWith("/")) {
                // 相对路径 → 拼接 baseUrl 的 scheme://host
                String base = baseUrl;
                int s = base.indexOf("://");
                if (s != -1) {
                    int e = base.indexOf('/', s + 3);
                    if (e != -1) base = base.substring(0, e);
                }
                return base + url;
            }
            return "";
        }
    }

    // meta refresh: content="0;url=..."
    p = body.indexOf("http-equiv=\"refresh\"");
    if (p == -1) p = body.indexOf("http-equiv='refresh'");
    if (p != -1) {
        p = body.indexOf("url=", p);
        if (p != -1) {
            p += 4;
            char q = (p < (int)body.length()) ? body.charAt(p) : 0;
            if (q == '"' || q == '\'') {
                p++;
                int e = body.indexOf(q, p);
                if (e == -1) e = body.length();
                return body.substring(p, e);
            }
            int e = body.indexOf(';', p);
            if (e == -1) e = body.length();
            return body.substring(p, e);
        }
    }
    return "";
}

// ==================== JSON 解析(容忍冒号两侧空格) ====================

static long jsonInt(const String& json, const String& key)
{
    int p = json.indexOf("\"" + key + "\"");
    if (p == -1) return -1;
    p = json.indexOf(':', p);
    if (p == -1) return -1;
    p++;
    while (p < (int)json.length() && (json.charAt(p) == ' ' || json.charAt(p) == '\t')) p++;
    int e = p;
    while (e < (int)json.length()) {
        char c = json.charAt(e);
        if (!((c >= '0' && c <= '9') || c == '-')) break;
        e++;
    }
    if (e == p) return -1;
    return json.substring(p, e).toInt();
}

static String jsonString(const String& json, const String& key)
{
    int p = json.indexOf("\"" + key + "\"");
    if (p == -1) return "";
    p = json.indexOf(':', p);
    if (p == -1) return "";
    p++;
    while (p < (int)json.length() && (json.charAt(p) == ' ' || json.charAt(p) == '\t')) p++;
    if (p >= (int)json.length() || json.charAt(p) != '"') return "";   // null/数字/布尔 → 空
    int e = json.indexOf('"', p + 1);
    if (e == -1) return "";
    return json.substring(p + 1, e);
}

static bool jsonBool(const String& json, const String& key)
{
    int p = json.indexOf("\"" + key + "\"");
    if (p == -1) return false;
    p = json.indexOf(':', p);
    if (p == -1) return false;
    int end = (p + 8 < (int)json.length()) ? p + 8 : json.length();
    return json.substring(p, end).indexOf("true") != -1;
}

// ==================== HTML 表单解析 ====================

static String tagAttr(const String& tag, const String& attr)
{
    String a1 = attr + "=\"";
    int p = tag.indexOf(a1);
    if (p != -1) {
        p += a1.length();
        int e = tag.indexOf('"', p);
        if (e == -1) return "";
        return tag.substring(p, e);
    }
    String a2 = attr + "='";
    p = tag.indexOf(a2);
    if (p != -1) {
        p += a2.length();
        int e = tag.indexOf('\'', p);
        if (e == -1) return "";
        return tag.substring(p, e);
    }
    return "";
}

// 取表单中指定 name 的 value(lt / execution)
static String getFormValue(const String& html, const String& name)
{
    int p = html.indexOf("name=\"" + name + "\"");
    if (p == -1) p = html.indexOf("name='" + name + "'");
    if (p == -1) return "";
    int tagStart = html.lastIndexOf('<', p);
    int tagEnd = html.indexOf('>', p);
    if (tagEnd == -1) tagEnd = html.length();
    return tagAttr(html.substring(tagStart, tagEnd), "value");
}

// 列出页面里每个 input 的 name(type), 用于诊断"提取不到 lt/execution"时
// 服务端到底给了什么页(二次认证页 / 错误页 / 已跳转的空壳页字段名完全不同)
static String formFieldNames(const String& html)
{
    String out;
    int p = 0;
    while ((p = html.indexOf("<input", p)) != -1) {
        int e = html.indexOf('>', p);
        if (e == -1) break;
        String tag = html.substring(p, e);
        String name = tagAttr(tag, "name");
        if (name.length()) {
            if (out.length()) out += " ";
            out += name + "(" + tagAttr(tag, "type") + ")";
        }
        p = e + 1;
    }
    return out.length() ? out : "(页面无带 name 的 input)";
}

// 收集登录页所有 hidden 字段(排除已手工处理的), 防止遗漏服务端要求的字段
static String collectHiddenFields(const String& html)
{
    String extra;
    int p = 0;
    while ((p = html.indexOf("<input", p)) != -1) {
        int e = html.indexOf('>', p);
        if (e == -1) break;
        String tag = html.substring(p, e);
        if (tagAttr(tag, "type") == "hidden") {
            String name = tagAttr(tag, "name");
            String value = tagAttr(tag, "value");
            if (name.length() &&
                name != "rsa" && name != "ul" && name != "pl" && name != "sl" &&
                name != "lt" && name != "execution" && name != "_eventId") {
                extra += "&" + name + "=" + value;
            }
        }
        p = e + 1;
    }
    return extra;
}

// 在页面/JSON 中寻找 SSO 登录页 URL(若服务端直接在响应中给出)
static String findSsoUrl(const String& res)
{
    int p = res.indexOf("https://sso.dlut.edu.cn/cas/login?");
    if (p != -1) {
        int e1 = res.indexOf('"', p);
        int e2 = res.indexOf('\'', p);
        int e3 = res.indexOf(' ', p);
        int e = e1;
        if (e == -1 || (e2 != -1 && e2 < e)) e = e2;
        if (e == -1 || (e3 != -1 && e3 < e)) e = e3;
        if (e == -1) e = res.length();
        return res.substring(p, e);
    }
    return "";
}

// ==================== 共享会话原语(门禁等子系统复用) ====================

String campusUrlEncode(const String& s) { return urlEncodeQuery(s); }
String campusHttp(const String& url, const String& method, const String& body,
                  const String& contentType, const String& extraHeaders)
{
    return httpRequest(url, method, body, contentType, extraHeaders);
}
void campusSaveCookies(const String& res) { saveCookie(res); }
String campusLocation(const String& res, const String& baseUrl)
{
    return getRedirectUrl(res, baseUrl);
}
String campusBody(const String& res) { return getBody(res); }
String campusCookie(const String& host, const String& name)
{
    return getCookieValue(host, name);
}

bool campusCasTicket(const String& service, String& ticketUrl)
{
    ticketUrl = "";
    // 门禁专用: 把 config.h 的 COOKIE_INPUT 灌成基础 cookie(门户登录不碰它)。
    // 已有服务端亲发的同名 CASTGC 时种子自动让路, 只在缺登录态时起作用。
    applySeedCookies();
    String loginUrl = String(SSO_HOST) + "/cas/login?service=" + urlEncodeQuery(service);

    // ① 已持 CASTGC: CAS 直接 302 回 service?ticket=ST-...
    String res = httpRequest(loginUrl, "GET", "", "", "");
    saveCookie(res);
    String loc = getRedirectUrl(res, loginUrl);
    if (loc.indexOf("ticket=") != -1) { ticketUrl = loc; return true; }

    // ② 未登录: 响应体本身就是登录表单(老版门禁脚本走的就是这条路),
    //    也可能再 302 一次到带 service 的 /cas/login 才给表单
    String html = getBody(res);
    if (loc.startsWith("http") && loc.indexOf("/cas/login") != -1 && loc != loginUrl) {
        loginUrl = loc;
        res = String();
        res = httpRequest(loginUrl, "GET", "", "", "");
        saveCookie(res);
        loc = getRedirectUrl(res, loginUrl);
        if (loc.indexOf("ticket=") != -1) { ticketUrl = loc; return true; }   // 页面自带票据
        html = getBody(res);
    }

    // ③ 提交账号密码(rsa 字段为 DES 输出, 明文 = 账号+密码+lt)
    String lt = getFormValue(html, "lt");
    String execution = getFormValue(html, "execution");
    String hiddenExtra = collectHiddenFields(html);
    Serial.println("[票据] 无登录态, 提交表单: lt=" + lt + " execution=" + execution);
    if (!lt.length() || !execution.length()) {
        Serial.println("[票据] 登录页无 lt/execution");
        printTrunc("票据登录页", html);
        return false;
    }
    // 缓冲区按 DES 最大输出长度分配(每 4 字节明文 → 16 个 hex), 见 campusLogin [6]
    String data = String(cfg.user) + cfg.pass + lt;
    const size_t rsaCap = (data.length() + 3) / 4 * 16 + 1;
    char* rsa = (char*)malloc(rsaCap);
    if (!rsa) {
        Serial.println("[票据] 内存不足无法加密");
        return false;
    }
    strEnc(data.c_str(), "1", "2", "3", rsa, rsaCap);
    String body = String("rsa=") + rsa +
                  "&ul=" + String(strlen(cfg.user)) +
                  "&pl=" + String(strlen(cfg.pass)) +
                  "&sl=0" +
                  "&lt=" + lt +
                  "&execution=" + execution +
                  "&_eventId=submit" +
                  hiddenExtra;
    printHeap("[票据] 提交登录");
    res = String();
    res = httpRequest(loginUrl, "POST", body, "application/x-www-form-urlencoded",
                      "Origin: " + String(SSO_HOST) + "\r\nReferer: " + loginUrl + "\r\n");
    free(rsa);
    saveCookie(res);                       // CASTGC
    loc = getRedirectUrl(res, loginUrl);
    if (loc.indexOf("ticket=") != -1) { ticketUrl = loc; return true; }

    // ④ 提交成功但 service 未回票: 凭刚拿到的 CASTGC 再请求一次换票
    res = String();
    res = httpRequest(String(SSO_HOST) + "/cas/login?service=" + urlEncodeQuery(service),
                      "GET", "", "", "Referer: " + loginUrl + "\r\n");
    saveCookie(res);
    loc = getRedirectUrl(res, loginUrl);
    if (loc.indexOf("ticket=") != -1) { ticketUrl = loc; return true; }
    Serial.println("[票据] 换票失败: " + loc);
    return false;
}

// ==================== 主流程 ====================

void campusInit()
{
    if (cfg.portalUrl[0])
        Serial.println("[NVS] 已加载保存的认证入口地址");
    campusSeedCookies(cfg.cookie);   // 空串时什么都不做
}

// 凭手上的 CASTGC 直接向 CAS 换门户用的票据, 不走登录表单:
//   GET sso/cas/login?service=<callbackAuthorize?session_state=<JSESSIONIDCAS>!<ms>&casDelegate=null>
//   → 302 callbackAuthorize?...&ticket=ST-...
// 抓包证实 callbackAuthorize 自己永不发 ticket, 只能这样把 CAS 引到签票分支;
// session_state = JSESSIONIDCAS 值 + "!" + 毫秒时间戳。
// 用于 [5b]: 带 cookie(config.h 的 COOKIE_INPUT 或上一次登录拿到的 CASTGC)时 CAS
// 不再回带 lt/execution 的表单页, 登录链改走这条路。
static bool casRedeemPortalTicket(const String& referer, String& ticketUrl)
{
    String jsid = getCookieValue(SSO_HOSTNAME, "JSESSIONIDCAS");
    String inner = String(SSO_HOST) + "/cas/oauth2.0/callbackAuthorize?casDelegate=null";
    if (jsid.length())
        inner = String(SSO_HOST) + "/cas/oauth2.0/callbackAuthorize?session_state=" +
                jsid + "!" + String(millis()) + "&casDelegate=null";
    String cbUrl = String(SSO_HOST) + "/cas/login?service=" + urlEncodeQuery(inner);
    Serial.println("    换票请求: " + cbUrl);
    Serial.println("    手上的 cookie: " + dumpCookies());
    printHeap("[5b] 换票");
    String extra = referer.length() ? ("Referer: " + referer + "\r\n") : String();
    String res = httpRequest(cbUrl, "GET", "", "", extra);
    saveCookie(res);
    String loc = getRedirectUrl(res, cbUrl);
    Serial.println("[5b] 换票跳转: " + loc);
    if (loc.startsWith("http") && loc.indexOf("ticket=") != -1) { ticketUrl = loc; return true; }
    return false;
}

bool campusLogin(CampusInfo &info)
{
    info = CampusInfo();
    gCookieCount = 0;
    gSessionId = "";
    gUserMac = "";
    gCustomPageId = "";
    gUserIp = "";
    gNasIp = "";
    applySeedCookies();   // jar 清空后重新灌回基础 cookie(重登也不丢二次认证兜底)
    Serial.println("\n================ 校园网登录开始 ================");

    // ---- [0] 探测认证跳转 ----
    String res = httpRequest(PROBE_URL, "GET", "", "", "");
    saveCookie(res);
    String loc = getRedirectUrl(res, String(PROBE_URL));
    String portalUrl;
    if (loc.startsWith("http")) {
        portalUrl = loc;
        Serial.println("[0] 收到认证跳转");
        Serial.println("    " + loc);
    } else {
        Serial.println("[0] 未收到认证跳转(设备可能已在线), 尝试使用已保存的认证地址");
        if (cfg.portalUrl[0]) portalUrl = cfg.portalUrl;
        if (!portalUrl.length()) {
            Serial.println("[0] 没有已保存的认证地址: 请保证设备处于未认证状态(掉线)后重启");
            printTrunc("探测响应", res);
            return false;
        }
    }
    settingsSet("portalUrl", portalUrl.c_str(), nullptr);   // 存 NVS, 供下次"已在线"时复用

    // ---- [1] eportal/index.jsp → 302 → portal-main(获得 sessionId) ----
    Serial.println("[1] 请求 eportal 入口...");
    res = httpRequest(portalUrl, "GET", "", "", "");
    saveCookie(res);
    loc = getRedirectUrl(res, portalUrl);
    if (!loc.startsWith("http")) {
        Serial.println("[1] eportal 入口未返回跳转");
        printTrunc("响应", res);
        return false;
    }
    gSessionId = getQueryParam(loc, "sessionId");
    gUserMac   = getQueryParam(loc, "userMac");
    gCustomPageId = getQueryParam(loc, "customPageId");
    gUserIp   = getQueryParam(loc, "userIp");
    gNasIp    = getQueryParam(loc, "nasIp");
    Serial.println("    sessionId=" + gSessionId);
    Serial.println("    userMac=" + gUserMac);
    Serial.println("    customPageId=" + gCustomPageId);
    Serial.println("    userIp=" + gUserIp + " nasIp=" + gNasIp);
    if (gSessionId.length() == 0) {
        Serial.println("[1] 未能解析 sessionId");
        return false;
    }
    String portalMainUrl = loc;

    // ---- [2] 加载 portal-main 页面 ----
    Serial.println("[2] 加载 portal-main 页面...");
    res = httpRequest(portalMainUrl, "GET", "", "", "");
    saveCookie(res);
    String ssoHint = findSsoUrl(res);

    // ---- [3] 模拟 SPA 初始化请求(顺带寻找 SSO 入口) ----
    Serial.println("[3] SPA 初始化请求...");
    res = httpRequest(String(AUTH_HOST) + "/sam/api/protected/samconfig/getOtherConfig",
                      "POST", "", "application/json",
                      "Origin: " + String(AUTH_HOST) + "\r\nisPortal: true\r\n");
    saveCookie(res);
    printTrunc("sam/getOtherConfig", getBody(res));
    if (!ssoHint.length()) ssoHint = findSsoUrl(res);

    String nodeBody = "{\"sessionId\":\"" + gSessionId + "\",\"userMac\":\"" + gUserMac + "\"}";
    res = httpRequest(String(AUTH_HOST) + "/eportal/workFlow/getCurrentNode",
                      "POST", nodeBody, "application/json",
                      "Origin: " + String(AUTH_HOST) + "\r\nisPortal: true\r\n");
    saveCookie(res);
    printTrunc("workFlow/getCurrentNode", getBody(res));
    if (getBody(res).indexOf("Precondition Failed") != -1) {
        // 新版服务端 DTO 不识别 userMac 字段, 仅用 sessionId 重试
        nodeBody = "{\"sessionId\":\"" + gSessionId + "\"}";
        res = httpRequest(String(AUTH_HOST) + "/eportal/workFlow/getCurrentNode",
                          "POST", nodeBody, "application/json",
                          "Origin: " + String(AUTH_HOST) + "\r\nisPortal: true\r\n");
        saveCookie(res);
        printTrunc("workFlow/getCurrentNode(重试)", getBody(res));
    }
    if (!ssoHint.length()) ssoHint = findSsoUrl(res);

    res = httpRequest(String(AUTH_HOST) + "/eportal/adaptor/getOnlineUserInfo?sessionId=" + gSessionId +
                      "&" + String(millis()) + "&version=this%20is%20a%20git-commit",
                      "GET", "", "",
                      "isPortal: true\r\n");
    printTrunc("getOnlineUserInfo", getBody(res));
    if (!ssoHint.length()) ssoHint = findSsoUrl(res);

    // ---- [4] 进入 SSO 登录页 ----
    String ssoLoginUrl;
    if (ssoHint.length() && ssoHint.indexOf("/cas/login") != -1) {
        ssoLoginUrl = ssoHint;
        Serial.println("[4] 从响应中直接获得 SSO 登录页 URL");
    } else {
        // 方案A: auth 侧 cas-sso 入口, 按抓包原样带全套参数
        // (flowSessionId/customPageId/userIp/userMac 缺一会 302 到 /login 死胡同,
        //  而 /login 页面会再下发一条杂散 JSESSIONID 污染门户会话)
        String prevLoc;
        String referer = portalMainUrl;
        printHeap("[4] SSO入口");
        if (gCustomPageId.length() && gUserIp.length()) {
            String entry = String(AUTH_HOST) + "/cas-sso/login?flowSessionId=" + gSessionId +
                           "&customPageId=" + gCustomPageId +
                           "&preview=false&appType=normal&language=zh-CN" +
                           "&timer=" + String(millis()) +
                           "&nasIp=" + gNasIp +
                           "&userIp=" + gUserIp +
                           "&userMac=" + gUserMac;
            for (int hop = 0; hop < 6 && !ssoLoginUrl.length(); hop++) {
                res = String();   // 释放上一个响应, 降低 TLS 握手期堆压力
                res = httpRequest(entry, "GET", "", "", "Referer: " + referer + "\r\n");
                saveCookie(res);
                loc = getRedirectUrl(res, entry);
                Serial.printf("[4] SSO跳转%d: %s\n", hop + 1, loc.c_str());
                if (getBody(res).length() > 0 && getBody(res).indexOf("<") != -1)
                    printTrunc("跳转页内容", getBody(res));
                if (!loc.startsWith("http")) break;
                if (loc.indexOf("/cas/login") != -1) { ssoLoginUrl = loc; break; }
                if (loc == entry || loc == prevLoc) break;   // 防循环
                prevLoc = entry;
                referer = entry;
                entry = loc;
            }
        } else {
            Serial.println("[4] 缺少 customPageId/userIp, 跳过方案A");
        }
        // 方案B: 直接访问 sso 侧 callbackAuthorize(未登录时跳到裸 /cas/login)
        if (!ssoLoginUrl.length()) {
            printHeap("[4] callbackAuthorize");
            res = String();
            res = httpRequest(String(SSO_HOST) + "/cas/oauth2.0/callbackAuthorize?casDelegate=null",
                              "GET", "", "", "Referer: " + referer + "\r\n");
            saveCookie(res);
            loc = getRedirectUrl(res, String(SSO_HOST) + "/cas/oauth2.0/callbackAuthorize?casDelegate=null");
            Serial.println("[4] callbackAuthorize 跳转: " + loc);
            if (loc.startsWith("http") && loc.indexOf("/cas/login") != -1) ssoLoginUrl = loc;
        }
    }
    if (!ssoLoginUrl.length() || ssoLoginUrl.indexOf("/cas/login") == -1) {
        Serial.println("[4] 无法获得 SSO 登录页 URL, 流程终止");
        return false;
    }
    Serial.println("[4] SSO 登录页: " + ssoLoginUrl);

    // ---- [5] 获取登录页, 提取 lt / execution ----
    printHeap("[5] 获取登录页");
    res = String();
    res = httpRequest(ssoLoginUrl, "GET", "", "", "");
    saveCookie(res);   // JSESSIONIDCAS 等
    loc = getRedirectUrl(res, ssoLoginUrl);
    String ticketUrl;
    if (loc.startsWith("http") && loc.indexOf("ticket=") != -1) {
        // 已持有有效登录态(CASTGC), CAS 直接签发票据, 跳过表单
        Serial.println("[5] 已有有效登录态, CAS 直接签发 ticket");
        ticketUrl = loc;
    } else {
        String lt = getFormValue(res, "lt");
        String execution = getFormValue(res, "execution");
        String hiddenExtra = collectHiddenFields(res);
        Serial.println("[5] lt=" + lt);
        Serial.println("[5] execution=" + execution);
        if (!lt.length() || !execution.length()) {
            // 页面上没有登录表单: 带着 cookie(含 config.h 的基础 cookie)时 CAS 认为
            // 已登录, 不再下发 lt/execution —— 那就直接凭 CASTGC 换票([5b])
            Serial.println("[5] 无登录表单(" + statusLine(res) + "), 尝试凭 CASTGC 直接换票");
            Serial.println("    跳转: " + (loc.length() ? loc : String("(无跳转)")));
            Serial.println("    页面字段: " + formFieldNames(getBody(res)));
            if (!casRedeemPortalTicket(ssoLoginUrl, ticketUrl)) {
                Serial.println("[5b] 换票失败: CASTGC 无效或已过期, 请重新粘贴浏览器 cookie");
                printTrunc("登录页正文", getBody(res));
                return false;
            }
            Serial.println("[5b] 换票成功, 跳过表单提交");
        }
        // 上面已换到票就不提交表单(走表单时 ticketUrl 必为空)
        if (!ticketUrl.length()) {

            // ---- [6] 加密并提交登录(rsa 字段加密与门禁 CAS 登录一致) ----
            // rsa 为 hex(DES) 输出, 长度 == 明文长度*4(精简版不填充),
            // 明文 = 账号+密码+lt。为避免 DES 输出超出栈缓冲导致堆/栈损坏,
            // 按最大可能长度分配(每4字节明文→16 hex), 且 strEnc 内部带边界保护。
            String data = String(cfg.user) + cfg.pass + lt;
            // 编译期求最大输出长度: (len/4)*16 + (len%4)*16 + '\0'
            const size_t rsaCap = (data.length() + 3) / 4 * 16 + 1;
            char* rsa = (char*)malloc(rsaCap);
            if (!rsa) {
                Serial.println("[6] 内存不足无法加密");
                return false;
            }
            strEnc(data.c_str(), "1", "2", "3", rsa, rsaCap);
            String body = String("rsa=") + rsa +
                          "&ul=" + String(strlen(cfg.user)) +
                          "&pl=" + String(strlen(cfg.pass)) +
                          "&sl=0" +
                          "&lt=" + lt +
                          "&execution=" + execution +
                          "&_eventId=submit" +
                          hiddenExtra;
            free(rsa);

            printHeap("[6] 提交登录");
            res = String();
            res = httpRequest(ssoLoginUrl, "POST", body, "application/x-www-form-urlencoded",
                              "Origin: " + String(SSO_HOST) + "\r\nReferer: " + ssoLoginUrl + "\r\n");
            saveCookie(res);   // CASTGC
            loc = getRedirectUrl(res, ssoLoginUrl);
            Serial.println("[6] CAS 登录跳转: " + loc);
            if (!loc.startsWith("http")) {
                Serial.println("[6] CAS 登录失败(账号密码错误或需要验证码)");
                printTrunc("登录响应", res);
                return false;
            }
            if (loc.indexOf("ticket=") != -1) {
                ticketUrl = loc;
            } else {
                // 登录已成功(CASTGC 已签发)但 service 缺省跳向门户:
                // 带 CASTGC 请求 /cas/login?service=<callbackAuthorize URL>, CAS 会直接
                // 302 到 service?ticket=ST-... (抓包第153→181行)
                // (抓包证实 session_state = JSESSIONIDCAS 值 + "!" + 毫秒时间戳;
                //  直接请求 callbackAuthorize 不会发 ticket, 它只当回调用)
                Serial.println("[6] 登录成功但无 ticket, 凭 CASTGC 走 /cas/login?service= 换票");
                Serial.println("    cookies: " + dumpCookies());
                String jsid = getCookieValue(SSO_HOSTNAME, "JSESSIONIDCAS");
                String inner = String(SSO_HOST) + "/cas/oauth2.0/callbackAuthorize?casDelegate=null";
                if (jsid.length())
                    inner = String(SSO_HOST) + "/cas/oauth2.0/callbackAuthorize?session_state=" +
                            jsid + "!" + String(millis()) + "&casDelegate=null";
                String cbUrl = String(SSO_HOST) + "/cas/login?service=" + urlEncodeQuery(inner);
                Serial.println("    请求: " + cbUrl);
                printHeap("[6b] 换票");
                res = String();
                res = httpRequest(cbUrl, "GET", "", "", "Referer: " + ssoLoginUrl + "\r\n");
                saveCookie(res);
                loc = getRedirectUrl(res, cbUrl);
                Serial.println("[6b] 换票跳转: " + loc);
                if (loc.startsWith("http") && loc.indexOf("ticket=") != -1) ticketUrl = loc;
                else printTrunc("换票响应", res);
            }
        }   // 结束 if (!ticketUrl.length()) —— [6] 表单提交
    }
    if (!ticketUrl.length()) {
        Serial.println("[6] 未能获得 ticket, 登录链中断");
        return false;
    }

    // ---- [7] OAuth 交换: 完全复刻成功抓包的交换链 ----
    // 抓包证实(注意 state 不能省略, 且需从 authorize 引导链中取得):
    //   ① GET /cas-sso/clientredirect?client_name=ssoOauth&service=<authenticate 完成页>
    //      带 auth 侧的 SESSION cookie → 302 到 sso/cas/oauth2.0/authorize?response_type=code&state=TST-...
    //   ② GET authorize(带 CASTGC) → 302 到 sso/cas/login?service=callbackAuthorize...
    //   ③ GET cas/login?service=...(带 CASTGC) → 302 到 callbackAuthorize?...&ticket=ST-xxx
    //   ④ GET callbackAuthorize?...ticket=ST-xxx → 302 到 auth/cas-sso/login?...&code=ST-xxx&state=<①的state>
    //   ⑤ GET cas-sso/login?...code=ST-xxx&state=... → 302 到 portal/entry/pc/authenticate;...?ticket=ST-yyy
    //   ⑥ GET authenticate → 200(建立 eportal 会话) → 之后 userOnline 才能真正上线。
    // 设备必须先走 ① 拿到 auth 侧 SESSION 会话与 OAuth state, 否则直接 /cas-sso/login?code=
    // 会让服务端把请求当作未建立会话而 302 到 auth/login 登录页。
    Serial.println("[7] OAuth 交换链(复刻抓包)...");

    // ① cas-sso/clientredirect → authorize(取得 state)
    const char* redirectService =
        "http://auth.dlut.edu.cn/portal/entry/pc/authenticate;flowParams=undefined;from=";
    String cr = String(AUTH_HOST) + "/cas-sso/clientredirect?client_name=ssoOauth" +
                "&accept-language=zh-CN&service=" + urlEncodeQuery(redirectService);
    printHeap("[7] clientredirect");
    res = String();
    res = httpRequest(cr, "GET", "", "", "");
    saveCookie(res);   // auth 侧 SESSION + PAC4JDELSESSION
    loc = getRedirectUrl(res, cr);
    Serial.println("[7] clientredirect→: " + loc);

    // ✓ 保存 OAuth state(来自 authorize 的 `state=TST-...` 参数, 原样传递用于交换)
    String oauthState = getQueryParam(loc, "state");
    Serial.println("[7] OAuth state: " + oauthState);

    // ② authorize(带 CASTGC/超时时间戳重刷 jsid) → ③ cas/login → ④ callbackAuthorize?ticket=
    if (!loc.startsWith("http") || loc.indexOf("authorize") == -1) {
        Serial.println("[7] clientredirect 未收敛到 authorize, 改用已取得的换票 ticket");
        loc = ticketUrl;
    }
    // ③④ 交换出带 ticket 的 callbackAuthorize URL: 复刻 [6b], 但基于这里的 loc
    res = String();
    res = httpRequest(loc, "GET", "", "", "");
    saveCookie(res);   // JSESSIONIDCAS / CASTGC
    loc = getRedirectUrl(res, loc);
    Serial.println("[7] 引导跳转: " + loc);
    if (loc.startsWith("http") && loc.indexOf("/cas/login") != -1) {
        // ③ 已登录 → ④ 换票
        String jsid = getCookieValue(SSO_HOSTNAME, "JSESSIONIDCAS");
        String inner = String(SSO_HOST) + "/cas/oauth2.0/callbackAuthorize?casDelegate=null";
        if (jsid.length())
            inner = String(SSO_HOST) + "/cas/oauth2.0/callbackAuthorize?session_state=" +
                    jsid + "!" + String(millis()) + "&casDelegate=null";
        String cbUrl = String(SSO_HOST) + "/cas/login?service=" + urlEncodeQuery(inner);
        res = String();
        res = httpRequest(cbUrl, "GET", "", "", "");
        saveCookie(res);
        loc = getRedirectUrl(res, cbUrl);
        Serial.println("[7] 换票→: " + loc + "");   // 应含 ticket=
    }
    if (!loc.startsWith("http") || loc.indexOf("ticket=") == -1) {
        Serial.println("[7] 引导链未取得 ticket, 用原换票 ticketUrl");
        loc = ticketUrl;
    }
    String stateKey = oauthState;   // 用 authorize 的原生 state(TST-...) 交换, 不用 session_state 推导

    // ⑦ 带 ticket/code 请求 auth cas-sso/login(使用前面取到的 state)
    // 必须带上: auth 侧 SESSION cookie(第一步已存) + state + code。三者缺一会
    // 被服务端当作未注册客户端而 302 回 auth/login。Referer 用 auth 门户源站。
    String exchange = String(AUTH_HOST) + "/cas-sso/login?client_name=ssoOauth" +
                      "&accept-language=zh-CN&code=" + getQueryParam(loc, "ticket");
    if (stateKey.length()) exchange += "&state=" + stateKey;
    exchange += "&casDelegate=null";
    printHeap("[7] 交换 code");
    res = String();
    res = httpRequest(exchange, "GET", "", "", "Referer: " + String(AUTH_HOST) + "/\r\n");
    saveCookie(res);   // SOURCEID_TGC / rg_objectid
    loc = getRedirectUrl(res, exchange);
    Serial.println("[7] 交换跳转: " + loc);
    if (!loc.startsWith("http")) {
        Serial.println("[7] OAuth 交换失败");
        printTrunc("交换响应", res);
        return false;
    }

    // ---- [8] authenticate 完成页(建立 eportal 会话) ----
    // 实测: GET portal/entry/pc/authenticate;...?ticket=ST-yyy 直接返回 200(门户 SPA 首页),
    // 不再有 302。若出现 302 则跟随一次; 200 即代表会话已在 eportal 侧建立。
    res = String();
    res = httpRequest(loc, "GET", "", "", "");
    saveCookie(res);
    String stepLoc = getRedirectUrl(res, loc);
    Serial.println("[8] authenticate 跳转: " + stepLoc);
    Serial.printf("[8] authenticate 状态: %s (%u 字节)\n",
                  res.startsWith("HTTP/1.1 200") ? "200 成功" : "非200", (unsigned)res.length());
    if (stepLoc.startsWith("http") && stepLoc.indexOf("/cas/login") == -1) {
        // 有后续 302 才跟随; 回到登录页(异常)则不跟随
        loc = stepLoc;
        res = String();
        res = httpRequest(loc, "GET", "", "", "");
        saveCookie(res);
        Serial.println("[8] authenticate 二次跟随: " + getRedirectUrl(res, loc));
        Serial.printf("[8] authenticate 二次状态: %s\n",
                      res.startsWith("HTTP/1.1 200") ? "200 成功" : "非200");
    }
    if (res.length() == 0) {
        Serial.println("[8] authenticate 无响应(会话可能未建立)");
        return false;
    }

    // ---- [9] authenticate 后按抓包补齐 SPA 引导序列(顺序与请求缺一不可) ----
    // 抓包证实: authenticate(200)后客户端依次请求
    //   getCustomizedPageConfigByExternalId → querySuccessPageCustomizedPageConfig →
    //   getOtherConfig → getOnlineUserInfo(此时已返回 userIndex) → queryTerminalInfo →
    //   getCurrentNode(flowKey=portal_auth) → querySuccessPageCustomizedPageConfig →
    //   userOnline(online:true) → getAccountInfo
    // 其中 queryTerminalInfo 与 portal_auth 的 getCurrentNode 把工作流从 authenticate
    // 推进到 finish; 缺了它们 userOnline 返回 online:false + 空 userIndex。
    if (res.length() == 0) {
        Serial.println("[8] authenticate 无响应(会话可能未建立)");
        return false;
    }

    res = httpRequest(String(AUTH_HOST) + "/sam/api/protected/eportal/getCustomizedPageConfigByExternalId",
                      "POST", "{\"externalId\":\"" + gCustomPageId + "\",\"type\":\"pc\"}",
                      "application/json",
                      "Origin: " + String(AUTH_HOST) + "\r\nisPortal: true\r\n");
    saveCookie(res);

    res = String();
    res = httpRequest(String(AUTH_HOST) + "/sam/api/protected/eportal/querySuccessPageCustomizedPageConfig",
                      "POST", "", "application/json",
                      "Origin: " + String(AUTH_HOST) + "\r\nisPortal: true\r\n");
    saveCookie(res);

    res = httpRequest(String(AUTH_HOST) + "/sam/api/protected/samconfig/getOtherConfig",
                      "POST", "", "application/json",
                      "Origin: " + String(AUTH_HOST) + "\r\nisPortal: true\r\n");
    saveCookie(res);
    Serial.printf("[9] getOtherConfig: code=%ld\n", jsonInt(getBody(res), "code"));

    // 确认在线身份已绑定(抓包此处 userIndex 已有值; 空则稍候重试一次)
    String preIndex;
    for (int retry = 0; retry < 2; retry++) {
        res = String();
        res = httpRequest(String(AUTH_HOST) + "/eportal/adaptor/getOnlineUserInfo?sessionId=" + gSessionId +
                          "&" + String(millis()) + "&version=this%20is%20a%20git-commit",
                          "GET", "", "", "isPortal: true\r\n");
        preIndex = jsonString(getBody(res), "userIndex");
        Serial.printf("[9] 上线前 userIndex=%s\n",
                      preIndex.length() ? preIndex.c_str() : "(空)");
        if (preIndex.length()) break;
        delay(500);
    }

    // queryTerminalInfo(工作流 authenticate → terminalLoad 的推进点)
    res = String();
    res = httpRequest(String(AUTH_HOST) + "/eportal/adaptor/queryTerminalInfo?sessionId=" + gSessionId +
                      "&macAddr=&" + String(millis()) + "&version=this%20is%20a%20git-commit",
                      "GET", "", "", "isPortal: true\r\n");

    // getCurrentNode(portal_auth, 推进到 finish)
    res = String();
    res = httpRequest(String(AUTH_HOST) + "/eportal/workFlow/getCurrentNode",
                      "POST", "{\"sessionId\":\"" + gSessionId + "\",\"flowKey\":\"portal_auth\"}",
                      "application/json",
                      "Origin: " + String(AUTH_HOST) + "\r\nisPortal: true\r\n");
    Serial.printf("[9] getCurrentNode: %s\n", jsonString(getBody(res), "currentNodePath").c_str());

    // 完成页配置(Referer 已是 finish 页)
    res = String();
    res = httpRequest(String(AUTH_HOST) + "/sam/api/protected/eportal/querySuccessPageCustomizedPageConfig",
                      "POST", "", "application/json",
                      "Origin: " + String(AUTH_HOST) + "\r\nisPortal: true\r\n");
    saveCookie(res);

    // ---- [10] 设备上线 ----
    String upBody = "{\"sessionId\":\"" + gSessionId + "\"}";
    res = httpRequest(String(AUTH_HOST) + "/eportal/network/userOnline",
                      "POST", upBody, "application/json",
                      "Origin: " + String(AUTH_HOST) +
                      "\r\nReferer: " + String(AUTH_HOST) + "/portal/entry/pc/finish;flowParams=undefined;from=\r\n" +
                      "isPortal: true\r\n");
    printTrunc("userOnline", getBody(res));
    String userIndex = jsonString(getBody(res), "userIndex");
    Serial.println("    userIndex=" + userIndex);
    if (!jsonBool(getBody(res), "online")) {
        Serial.println("[10] 注意: userOnline 未返回 online=true(可能已在线)");
    }

    res = String();
    res = httpRequest(String(AUTH_HOST) + "/sam/api/protected/eportal/querySuccessPageCustomizedPageConfig",
                      "POST", "", "application/json",
                      "Origin: " + String(AUTH_HOST) + "\r\nisPortal: true\r\n");
    saveCookie(res);

    // ---- [11] 获取账户信息(余额/剩余流量/在线设备) ----
    campusRefreshAccount(info);

    Serial.println("================ 校园网登录结束 ================");
    return true;
}

bool campusRefreshAccount(CampusInfo &info)
{
    // 抓包原样: mode 为空字符串。必须带 /eportal/ 作用域 JSESSIONID(按 host+path 匹配)
    // 才会返回 套餐&余额/剩余流量/在线设备; 否则 data 全为 null。
    String accBody = "{\"sessionId\":\"" + gSessionId + "\",\"mode\":\"\"}";
    String accRes = httpRequest(String(AUTH_HOST) + "/eportal/operator/getAccountInfo",
                                "POST", accBody, "application/json",
                                "Origin: " + String(AUTH_HOST) +
                                "\r\nReferer: " + String(AUTH_HOST) + "/portal/entry/pc/finish;flowParams=undefined;from=\r\n" +
                                "isPortal: true\r\n");
    String accJson = getBody(accRes);
    printTrunc("getAccountInfo", accJson);
    info.userName = jsonString(accJson, "name");
    info.online = true;

    int p = 0;
    while ((p = accJson.indexOf("\"title\"", p)) != -1) {
        String title = jsonString(accJson.substring(p), "title");
        String content = jsonString(accJson.substring(p), "content");
        p += 8;
        if (!title.length()) continue;
        Serial.printf("    %s: %s\n", title.c_str(), content.c_str());
        if (title.indexOf("余额") != -1) info.balance = content;
        else if (title.indexOf("流量") != -1) info.traffic = content;
        else if (title.indexOf("设备") != -1) info.devices = content;
    }

    bool ok = info.balance.length() || info.traffic.length() || info.devices.length();
    if (!ok) {
        Serial.println("[11] 未能解析账户信息(可能 /eportal/ JSESSIONID 缺失, 会话已失效)");
    }
    return ok;
}

bool campusCheckOnline()
{
    if (!gSessionId.length()) return false;
    Serial.println("[定时] 检测在线状态...");
    String res = httpRequest(String(AUTH_HOST) + "/eportal/adaptor/getOnlineUserInfo?sessionId=" + gSessionId +
                             "&" + String(millis()) + "&version=this%20is%20a%20git-commit",
                             "GET", "", "", "isPortal: true\r\n");
    String body = getBody(res);
    printTrunc("checkOnline", body);
    if (body.length() == 0) return false;          // 被拦截(无会话) → 掉线
    int p = body.indexOf("\"online\"");
    if (p == -1) return true;                      // 会话有效但无 online 字段 → 视为在线
    return jsonBool(body, "online");
}
